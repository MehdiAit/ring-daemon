/*
 *  Copyright (C) 2011-2015 Savoir-Faire Linux Inc.
 *  Author: Rafaël Carré <rafael.carre@savoirfairelinux.com>
 *  Author: Vivien Didelot <vivien.didelot@savoirfairelinux.com>
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

#include "logger.h"
#include "../video_device.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ring { namespace video {

class VideoDeviceImpl {
    public:
        /**
         * @throw std::runtime_error
         */
        VideoDeviceImpl(const std::string& path);

        std::string device;
        std::string name;

        std::vector<std::string> getChannelList() const;
        std::vector<std::string> getSizeList(const std::string& channel) const;
        std::vector<std::string> getRateList(const std::string& channel, const std::string& size) const;

        VideoSettings getSettings() const;
        void applySettings(VideoSettings settings);

        DeviceParams getDeviceParams() const;
};

VideoDeviceImpl::VideoDeviceImpl(const std::string& path) :
    device(path), name()
{
    // Set default settings
    applySettings(VideoSettings());
}

std::vector<std::string> VideoDeviceImpl::getChannelList() const
{
}

std::vector<std::string>
VideoDeviceImpl::getSizeList(const std::string& channel) const
{
}

std::vector<std::string>
VideoDeviceImpl::getRateList(const std::string& channel, const std::string& size) const
{
}

void
VideoDeviceImpl::applySettings(VideoSettings settings)
{
}

VideoSettings
VideoDeviceImpl::getSettings() const
{
    VideoSettings settings;
    settings.name = name;
    return settings;
}

DeviceParams
VideoDeviceImpl::getDeviceParams() const
{
    DeviceParams params;
    params.input = device;
    params.format = "android";
    return params;
}

VideoDevice::VideoDevice(const std::string& path) :
    deviceImpl_(new VideoDeviceImpl(path))
{
    //node_ = path;
    name = deviceImpl_->name;
}

void
VideoDevice::applySettings(VideoSettings settings)
{
    deviceImpl_->applySettings(settings);
}

VideoSettings
VideoDevice::getSettings() const
{
    return deviceImpl_->getSettings();
}

DeviceParams
VideoDevice::getDeviceParams() const
{
    return deviceImpl_->getDeviceParams();
}

DRing::VideoCapabilities
VideoDevice::getCapabilities() const
{
    DRing::VideoCapabilities cap;

    for (const auto& chan : deviceImpl_->getChannelList())
        for (const auto& size : deviceImpl_->getSizeList(chan))
            cap[chan][size] = deviceImpl_->getRateList(chan, size);

    return cap;
}

VideoDevice::~VideoDevice()
{}

}} // namespace ring::video