/*
 *  Copyright (C) 2004-2013 Savoir-Faire Linux Inc.
 *
 *  Author: Patrick Keroulas  <patrick.keroulas@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *  Additional permission under GNU GPL version 3 section 7:
 *
 *  If you modify this program, or any covered work, by linking or
 *  combining it with the OpenSSL project's OpenSSL library (or a
 *  modified version of that library), containing parts covered by the
 *  terms of the OpenSSL or SSLeay licenses, Savoir-Faire Linux Inc.
 *  grants you additional permission to convey the resulting work.
 *  Corresponding Source for a non-source form of such a combination
 *  shall include the source code for the parts of OpenSSL used as well
 *  as that of the covered work.
 */


#include "sippresence.h"
#include "logger.h"
#include "manager.h"
#include "client/client.h"
#include "client/presencemanager.h"
#include "sipaccount.h"
#include "sip_utils.h"
#include "pres_sub_server.h"
#include "pres_sub_client.h"
#include "sipvoiplink.h"

#define MAX_N_SUB_SERVER 20
#define MAX_N_SUB_CLIENT 20

SIPPresence::SIPPresence(SIPAccount *acc)
    : publish_sess_()
    , status_data_()
    , enabled_(true)
    , acc_(acc)
    , sub_server_list_()  //IP2IP context
    , sub_client_list_()
    , mutex_()
    , mutex_nesting_level_()
    , mutex_owner_()
    , cp_()
    , pool_()
{
    /* init default status */
    updateStatus(true, "Available");

    /* init pool */
    pj_caching_pool_init(&cp_, &pj_pool_factory_default_policy, 0);
    pool_ = pj_pool_create(&cp_.factory, "pres", 1000, 1000, NULL);

    /* Create mutex */
    if (pj_mutex_create_recursive(pool_, "pres", &mutex_) != PJ_SUCCESS)
        ERROR("Unable to create mutex");
}


SIPPresence::~SIPPresence()
{
    /* Flush the lists */
    for (const auto & c : sub_client_list_)
        removePresSubClient(c) ;

    for (const auto & s : sub_server_list_)
        removePresSubServer(s);
}

SIPAccount * SIPPresence::getAccount() const
{
    return acc_;
}

pjsip_pres_status * SIPPresence::getStatus()
{
    return &status_data_;
}

int SIPPresence::getModId() const
{
    return ((SIPVoIPLink*)(acc_->getVoIPLink()))->getModId();
}

pj_pool_t*  SIPPresence::getPool() const
{
    return pool_;
}

void SIPPresence::enable(bool flag)
{
    enabled_ = flag;
}

void SIPPresence::updateStatus(bool status, const std::string &note)
{
    //char* pj_note  = (char*) pj_pool_alloc(pool_, "50");

    pjrpid_element rpid = {
        PJRPID_ELEMENT_TYPE_PERSON,
        CONST_PJ_STR("20"),
        PJRPID_ACTIVITY_UNKNOWN,
        pj_str((char *) note.c_str())
    };

    /* fill activity if user not available. */
    if (note == "away")
        rpid.activity = PJRPID_ACTIVITY_AWAY;
    else if (note == "busy")
        rpid.activity = PJRPID_ACTIVITY_BUSY;
    else // TODO: is there any other possibilities
        DEBUG("Presence : no activity");

    pj_bzero(&status_data_, sizeof(status_data_));
    status_data_.info_cnt = 1;
    status_data_.info[0].basic_open = status;
    std::string tuple_id = std::to_string(rand()%1000);
    status_data_.info[0].id = CONST_PJ_STR(tuple_id.c_str());
    pj_memcpy(&status_data_.info[0].rpid, &rpid, sizeof(pjrpid_element));
    /* "contact" field is optionnal */
}

void SIPPresence::sendPresence(bool status, const std::string &note)
{
    updateStatus(status, note);

    if (not enabled_)
        return;

    if (acc_->isIP2IP())
        notifyPresSubServer(); // to each subscribers
    else
        publish(this); // to the PBX server
}


void SIPPresence::reportPresSubClientNotification(const std::string& uri, pjsip_pres_status * status)
{
    /* Update our info. See pjsua_buddy_get_info() for additionnal ideas*/
    const std::string acc_ID = acc_->getAccountID();
    const std::string basic(status->info[0].basic_open ? "open" : "closed");
    const std::string note(status->info[0].rpid.note.ptr, status->info[0].rpid.note.slen);
    DEBUG(" Received status of PresSubClient  %s(acc:%s): status=%s note=%s", uri.c_str(), acc_ID.c_str(),(status->info[0].basic_open ? "open" : "closed"), note.c_str());
    /* report status to client signal */
    Manager::instance().getClient()->getPresenceManager()->newBuddyNotification(acc_ID, uri, status->info[0].basic_open, note);
}

void SIPPresence::subscribeClient(const std::string& uri, bool flag)
{
    /* Check if the buddy was already subscribed */
    for (const auto & c : sub_client_list_) {
        if (c->getURI() == uri) {
            DEBUG("-PresSubClient:%s exists in the list. Replace it.", uri.c_str());
            delete c;
            removePresSubClient(c);
            break;
        }
    }

    if (sub_client_list_.size() >= MAX_N_SUB_CLIENT) {
        WARN("Can't add PresSubClient, max number reached.");
        return;
    }

    if (flag) {
        PresSubClient *c = new PresSubClient(uri, this);

        if (!(c->subscribe())) {
            WARN("Failed send subscribe.");
            delete c;
        }

        // the buddy has to be accepted before being added in the list
    }
}

void SIPPresence::addPresSubClient(PresSubClient *c)
{
    if (sub_client_list_.size() < MAX_N_SUB_CLIENT) {
        sub_client_list_.push_back(c);
        DEBUG("New Presence_subscription_client added (list[%i]).", sub_client_list_.size());
    } else {
        WARN("Max Presence_subscription_client is reach.");
        // let the client alive //delete c;
    }
}

void SIPPresence::removePresSubClient(PresSubClient *c)
{
    DEBUG("Presence_subscription_client removed from the buddy list.");
    sub_client_list_.remove(c);
}

void SIPPresence::approvePresSubServer(const std::string& uri, bool flag)
{
    for (const auto & s : sub_server_list_) {
        if (s->matches((char *) uri.c_str())) {
            s->approve(flag);
            // return; // 'return' would prevent multiple-time subscribers from spam
        }
    }
}


void SIPPresence::addPresSubServer(PresSubServer *s)
{
    if (sub_server_list_.size() < MAX_N_SUB_SERVER) {
        sub_server_list_.push_back(s);
    } else {
        WARN("Max Presence_subscription_server is reach.");
        // let de server alive // delete s;
    }
}

void SIPPresence::removePresSubServer(PresSubServer *s)
{
    sub_server_list_.remove(s);
    DEBUG("Presence_subscription_server removed");
}

void SIPPresence::notifyPresSubServer()
{
    DEBUG("Iterating through Presence_subscription_server:");

    for (const auto & s : sub_server_list_)
        s->notify();
}

void SIPPresence::lock()
{
    pj_mutex_lock(mutex_);
    mutex_owner_ = pj_thread_this();
    ++mutex_nesting_level_;
}

void SIPPresence::unlock()
{
    if (--mutex_nesting_level_ == 0)
        mutex_owner_ = NULL;

    pj_mutex_unlock(mutex_);
}

void SIPPresence::fillDoc(pjsip_tx_data *tdata, const pres_msg_data *msg_data)
{

    if (tdata->msg->type == PJSIP_REQUEST_MSG) {
        const pj_str_t STR_USER_AGENT = CONST_PJ_STR("User-Agent");
        std::string useragent(acc_->getUserAgentName());
        pj_str_t pJuseragent = pj_str((char*) useragent.c_str());
        pjsip_hdr *h = (pjsip_hdr*) pjsip_generic_string_hdr_create(tdata->pool, &STR_USER_AGENT, &pJuseragent);
        pjsip_msg_add_hdr(tdata->msg, h);
    }

    if (msg_data == NULL)
        return;

    const pjsip_hdr *hdr;
    hdr = msg_data->hdr_list.next;

    while (hdr && hdr != &msg_data->hdr_list) {
        pjsip_hdr *new_hdr;
        new_hdr = (pjsip_hdr*) pjsip_hdr_clone(tdata->pool, hdr);
        DEBUG("adding header", new_hdr->name.ptr);
        pjsip_msg_add_hdr(tdata->msg, new_hdr);
        hdr = hdr->next;
    }

    if (msg_data->content_type.slen && msg_data->msg_body.slen) {
        pjsip_msg_body *body;
        const pj_str_t type = CONST_PJ_STR("application");
        const pj_str_t subtype = CONST_PJ_STR("pidf+xml");
        body = pjsip_msg_body_create(tdata->pool, &type, &subtype, &msg_data->msg_body);
        tdata->msg->body = body;
    }
}

static const pjsip_publishc_opt my_publish_opt = {true}; // this is queue_request

/*
 * Client presence publication callback.
 */
void
SIPPresence::publish_cb(struct pjsip_publishc_cbparam *param)
{
    SIPPresence *pres = (SIPPresence*) param->token;

    if (param->code / 100 != 2 || param->status != PJ_SUCCESS) {

        pjsip_publishc_destroy(param->pubc);
        pres->publish_sess_ = NULL;
        std::string error = std::to_string(param->code) +" / "+ std::string(param->reason.ptr,param->reason.slen);

        if (param->status != PJ_SUCCESS) {
            char errmsg[PJ_ERR_MSG_SIZE];
            pj_strerror(param->status, errmsg, sizeof(errmsg));
            ERROR("Client (PUBLISH) failed, status=%d, msg=%s", param->status, errmsg);
            Manager::instance().getClient()->getPresenceManager()->serverError(
                    pres->getAccount()->getAccountID(),
                    error,
                    errmsg);

        } else if (param->code == 412) {
            /* 412 (Conditional Request Failed)
             * The PUBLISH refresh has failed, retry with new one.
             */
            WARN("Publish retry.");
            publish(pres);
        } else if ((param->code == PJSIP_SC_BAD_EVENT) || (param->code == PJSIP_SC_NOT_IMPLEMENTED)){ //489 or 501
            ERROR("Client (PUBLISH) failed (%s)",error.c_str());
            Manager::instance().getClient()->getPresenceManager()->serverError(
                    pres->getAccount()->getAccountID(),
                    error,
                    "Publish not supported.");
        }

    } else {
        if (param->expiration < 1) {
            /* Could happen if server "forgot" to include Expires header
             * in the response. We will not renew, so destroy the pubc.
             */
            pjsip_publishc_destroy(param->pubc);
            pres->publish_sess_ = NULL;
        }
    }
}

/*
 * Send PUBLISH request.
 */
pj_status_t
SIPPresence::send_publish(SIPPresence * pres, pj_bool_t active)
{
    pjsip_tx_data *tdata;
    pj_status_t status;

    DEBUG("Send %sPUBLISH (%s).", (active ? "" : "un-"), pres->getAccount()->getAccountID().c_str());

    SIPAccount * acc = pres->getAccount();
    std::string contactWithAngles =  acc->getFromUri();
    contactWithAngles.erase(contactWithAngles.find('>'));
    int semicolon = contactWithAngles.find_first_of(":");
    std::string contactWithoutAngles = contactWithAngles.substr(semicolon + 1);
//    pj_str_t contact = pj_str(strdup(contactWithoutAngles.c_str()));
//    pj_memcpy(&status_data.info[0].contact, &contt, sizeof(pj_str_t));;

    /* Create PUBLISH request */
    if (active) {
        char *bpos;
        pj_str_t entity;

        status = pjsip_publishc_publish(pres->publish_sess_, PJ_TRUE, &tdata);

        if (status != PJ_SUCCESS) {
            ERROR("Error creating PUBLISH request", status);
            goto on_error;
        }

        pj_str_t from = pj_str(strdup(acc->getFromUri().c_str()));

        if ((bpos = pj_strchr(&from, '<')) != NULL) {
            char *epos = pj_strchr(&from, '>');

            if (epos - bpos < 2) {
                pj_assert(!"Unexpected invalid URI");
                status = PJSIP_EINVALIDURI;
                goto on_error;
            }

            entity.ptr = bpos + 1;
            entity.slen = epos - bpos - 1;
        } else {
            entity = from;
        }

        /* Create and add PIDF message body */
        status = pjsip_pres_create_pidf(tdata->pool, pres->getStatus(),
                                        &entity, &tdata->msg->body);

        if (status != PJ_SUCCESS) {
            ERROR("Error creating PIDF for PUBLISH request");
            pjsip_tx_data_dec_ref(tdata);
            goto on_error;
        }

    } else {
        WARN("Unpublish is not implemented.");
    }


    pres_msg_data msg_data;
    pj_bzero(&msg_data, sizeof(msg_data));
    pj_list_init(&msg_data.hdr_list);
    pjsip_media_type_init(&msg_data.multipart_ctype, NULL, NULL);
    pj_list_init(&msg_data.multipart_parts);

    pres->fillDoc(tdata, &msg_data);

    /* Send the PUBLISH request */
    status = pjsip_publishc_send(pres->publish_sess_, tdata);

    if (status == PJ_EPENDING) {
        WARN("Previous request is in progress, ");
    } else if (status != PJ_SUCCESS) {
        ERROR("Error sending PUBLISH request");
        goto on_error;
    }

    return PJ_SUCCESS;

on_error:

    if (pres->publish_sess_) {
        pjsip_publishc_destroy(pres->publish_sess_);
        pres->publish_sess_ = NULL;
    }

    return status;
}


/* Create client publish session */
pj_status_t
SIPPresence::publish(SIPPresence *pres)
{
    pj_status_t status;
    const pj_str_t STR_PRESENCE = CONST_PJ_STR("presence");
    SIPAccount * acc = pres->getAccount();
    pjsip_endpoint *endpt = ((SIPVoIPLink*) acc->getVoIPLink())->getEndpoint();

    /* Create and init client publication session */

    /* Create client publication */
    status = pjsip_publishc_create(endpt, &my_publish_opt,
                                   pres, &publish_cb,
                                   &pres->publish_sess_);

    if (status != PJ_SUCCESS) {
        pres->publish_sess_ = NULL;
        ERROR("Failed to create a publish seesion.");
        return status;
    }

    /* Initialize client publication */
    pj_str_t from = pj_str(strdup(acc->getFromUri().c_str()));
    status = pjsip_publishc_init(pres->publish_sess_, &STR_PRESENCE, &from, &from, &from, 0xFFFF);

    if (status != PJ_SUCCESS) {
        ERROR("Failed to init a publish session");
        pres->publish_sess_ = NULL;
        return status;
    }

    /* Add credential for authentication */
    if (acc->hasCredentials() and pjsip_publishc_set_credentials(pres->publish_sess_, acc->getCredentialCount(), acc->getCredInfo()) != PJ_SUCCESS) {
        ERROR("Could not initialize credentials for invite session authentication");
        return status;
    }

    /* Set route-set */
    if (acc->hasServiceRoute())
        pjsip_regc_set_route_set(acc->getRegistrationInfo(), sip_utils::createRouteSet(acc->getServiceRoute(), pres->getPool()));

    /* Send initial PUBLISH request */
    status = send_publish(pres, PJ_TRUE);

    if (status != PJ_SUCCESS)
        return status;

    return PJ_SUCCESS;
}