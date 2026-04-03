/*******************************************************************************
  Copyright(c) 2024 QHY. All rights reserved.

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Library General Public
 License version 2 as published by the Free Software Foundation.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Library General Public License for more details.

 You should have received a copy of the GNU Library General Public License
 along with this library; see the file COPYING.LIB.  If not, write to
 the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 Boston, MA 02110-1301, USA.
*******************************************************************************/

#include "qhy_mount_base.h"
#include <connectionplugins/connectionserial.h>
#include <unistd.h>
#include <libnova/ln_types.h>
#include <cmath>
#include <cstring>

QHYMountBase::QHYMountBase() : EQMod()
{
    setTelescopeConnection(CONNECTION_SERIAL);
}

const char * QHYMountBase::getDefaultName()
{
    return "QHY Mount";
}

bool QHYMountBase::initProperties()
{
    const bool ok = EQMod::initProperties();

    if (!ok)
        return false;

    GoHomeSP[0].fill("SLEWHOME", "GoHome", ISS_OFF);
    GoHomeSP.fill(getDeviceName(), "TELESCOPE_HOME", "GoHome", MAIN_CONTROL_TAB, IP_RW, ISR_ATMOST1, 0, IPS_IDLE);

    return true;
}

bool QHYMountBase::ReadScopeStatus()
{
    const bool result = EQMod::ReadScopeStatus();

    if (!result || !m_CustomHomeActive || mount == nullptr)
        return result;

    if (gotoInProgress() || TrackState == SCOPE_SLEWING)
        return result;

    try
    {
        if (!mount->IsRARunning() && !mount->IsDERunning())
        {
            if (TrackState == SCOPE_TRACKING)
                SetTrackEnabled(false);
            SetParked(false);
            m_CustomHomeActive = false;
            GoHomeSP.setState(IPS_IDLE);
            GoHomeSP.reset();
            GoHomeSP.apply("QHY GoHome completed. Tracking disabled at home position.");
            LOG_INFO("QHY custom home completed. Tracking disabled at home position.");
        }
    }
    catch (EQModError &e)
    {
        if (!(e.DefaultHandleException(this)))
            LOGF_WARN("QHY custom home completion handling failed: %s", e.message);
    }

    return result;
}

bool QHYMountBase::updateProperties()
{
    bool result = EQMod::updateProperties();

    if (!result)
        return false;

    if (isConnected())
        defineProperty(GoHomeSP);
    else
        deleteProperty(GoHomeSP);

    if (result && isConnected() && mount)
    {
        try
        {
            mount->SyncTimeAndTimezone();
            LOG_INFO("QHY Mount initial time synchronization completed with system time");
        }
        catch (EQModError &e)
        {
            LOGF_WARN("QHY Mount initial time synchronization failed: %s", e.message);
        }

        try
        {
            mount->SyncLocationCoordinates();
            LOG_INFO("QHY Mount initial location synchronization completed");
        }
        catch (EQModError &e)
        {
            LOGF_WARN("QHY Mount initial location synchronization failed: %s", e.message);
        }
    }

    return result;
}

bool QHYMountBase::Abort()
{
    m_CustomHomeActive = false;
    GoHomeSP.setState(IPS_IDLE);
    GoHomeSP.reset();
    GoHomeSP.apply();
    return EQMod::Abort();
}

bool QHYMountBase::ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int n)
{
    if (strcmp(dev, getDeviceName()) == 0 && GoHomeSP.isNameMatch(name))
    {
        GoHomeSP.update(states, names, n);

        if (GoHomeSP[0].getState() != ISS_ON)
        {
            GoHomeSP.reset();
            GoHomeSP.setState(IPS_IDLE);
            GoHomeSP.apply();
            return true;
        }

        if (m_CustomHomeActive)
        {
            LOG_WARN("Aborting QHY GoHome.");
            Abort();
            return true;
        }

        if (TrackState == SCOPE_SLEWING || TrackState == SCOPE_PARKING)
        {
            GoHomeSP.reset();
            GoHomeSP.setState(IPS_IDLE);
            GoHomeSP.apply("Can not start GoHome while mount motion is in progress.");
            LOG_WARN("Can not start QHY GoHome while mount motion is in progress.");
            return true;
        }

        const double jd = getJulianDate();
        const double lst = getLst(jd, getLongitude());
        constexpr double homeHA = 6.0;

        double targetRA = std::fmod(lst - homeHA + 24.0, 24.0);
        if (targetRA < 0)
            targetRA += 24.0;

        const double targetDEC = (getLatitude() >= 0.0) ? 90.0 : -90.0;

        LOGF_INFO("QHY GoHome requested: current RA=%g DEC=%g, target HA=%g => target RA=%g DEC=%g",
                  currentRA, currentDEC, homeHA, targetRA, targetDEC);

        GoHomeSP.setState(IPS_BUSY);
        GoHomeSP.apply("QHY GoHome slew started.");

        suppressNextGotoTracking = true;
        const bool gotoStarted = Goto(targetRA, targetDEC);
        if (!gotoStarted)
        {
            suppressNextGotoTracking = false;
            GoHomeSP.reset();
            GoHomeSP.setState(IPS_ALERT);
            GoHomeSP.apply("QHY GoHome failed to start.");
            return true;
        }

        m_CustomHomeActive = true;
        return true;
    }

    return EQMod::ISNewSwitch(dev, name, states, names, n);
}

bool QHYMountBase::updateTime(ln_date *utc, double utc_offset)
{
    LOGF_INFO("KStars trying to set time: %04d-%02d-%02d %02d:%02d:%02.0f UTC, offset: %.2f hours",
              utc->years, utc->months, utc->days, utc->hours, utc->minutes, utc->seconds, utc_offset);

    bool result = EQMod::updateTime(utc, utc_offset);

    if (result && isConnected() && mount)
    {
        try
        {
            mount->SyncTimeAndTimezone();
            LOG_INFO("QHY Mount time synchronization completed with system time (ignoring KStars time)");
        }
        catch (EQModError &e)
        {
            LOGF_WARN("QHY Mount time synchronization failed: %s", e.message);
        }
    }

    return result;
}

bool QHYMountBase::updateLocation(double latitude, double longitude, double elevation)
{
    bool result = EQMod::updateLocation(latitude, longitude, elevation);

    if (result && isConnected() && mount)
    {
        try
        {
            mount->SyncLocationCoordinates(latitude, longitude, elevation);

            LOGF_INFO("QHY Mount location synchronization completed: Lat %.6f°, Lon %.6f°, Elev %.0fm",
                      latitude, longitude, elevation);
        }
        catch (EQModError &e)
        {
            LOGF_WARN("QHY Mount location synchronization failed: %s", e.message);
        }
    }

    return result;
}
