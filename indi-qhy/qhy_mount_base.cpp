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

void QHYMountBase::resetCustomMotionState()
{
    // Custom Home/Park completion is handled entirely in this class, so the next
    // normal Goto should behave like a regular slew and be free to re-enable tracking.
    suppressNextGotoTracking = false;
    gotoparams.completed     = true;

    if (CanControlTrack())
        SetTrackEnabled(false);
    else
    {
        TrackState         = SCOPE_IDLE;
        RememberTrackState = TrackState;
    }
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
    if (mount == nullptr)
        return false;

    if (m_CustomHomeActive || m_CustomParkActive)
    {
        try
        {
            currentRA  = mount->GetRAEncoder();
            currentDEC = mount->GetDEEncoder();
            NewRaDec(currentRA, currentDEC);

            const bool raRunning = mount->IsRARunning();
            const bool deRunning = mount->IsDERunning();
            if (raRunning || deRunning)
                return true;

            // Make completion idempotent and avoid re-entering the generic EQMod goto/tracking
            // completion path for QHY custom semantic motions.
            try
            {
                mount->StopRA();
                mount->StopDE();
            }
            catch (EQModError &e)
            {
                LOGF_WARN("QHY custom motion stop-on-complete failed: %s", e.message);
            }

            resetCustomMotionState();

            if (m_CustomHomeActive)
            {
                if (isParked())
                    SetParked(false);

                m_CustomHomeActive = false;
                GoHomeSP.setState(IPS_IDLE);
                GoHomeSP.reset();
                GoHomeSP.apply("QHY GoHome completed. Tracking disabled at home position.");
                LOG_INFO("QHY custom home completed. Tracking disabled at home position.");
            }

            if (m_CustomParkActive)
            {
                m_CustomParkActive = false;
                SetParked(true);
                LOG_INFO("QHY custom park completed.");
            }

            return true;
        }
        catch (EQModError &e)
        {
            return e.DefaultHandleException(this);
        }
    }

    const bool result = EQMod::ReadScopeStatus();

    if (!result)
        return result;

    try
    {
        // No custom handling here. Custom Home/Park is handled before EQMod::ReadScopeStatus()
        // so the generic iterative goto/tracking completion logic can not interfere.
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
    m_CustomParkActive = false;
    GoHomeSP.setState(IPS_IDLE);
    GoHomeSP.reset();
    GoHomeSP.apply();
    resetCustomMotionState();
    return EQMod::Abort();
}

bool QHYMountBase::Park()
{
    if (isParked())
        return true;

    if (TrackState == SCOPE_SLEWING || m_CustomHomeActive || m_CustomParkActive)
    {
        LOG_WARN("Can not start QHY custom park while another motion is in progress.");
        return false;
    }

    try
    {
        if (!mount->ExecuteQHYPark())
            return false;
    }
    catch (EQModError &e)
    {
        return e.DefaultHandleException(this);
    }

    m_CustomParkActive = true;
    m_CustomHomeActive = false;
    suppressNextGotoTracking = false;
    gotoparams.completed     = true;
    TrackState = SCOPE_PARKING;
    RememberTrackState = TrackState;
    LOG_INFO("QHY custom park started.");
    return true;
}

bool QHYMountBase::SetCurrentPark()
{
    try
    {
        if (!mount->ExecuteQHYSetPark())
            return false;
    }
    catch (EQModError &e)
    {
        return e.DefaultHandleException(this);
    }

    LOG_INFO("QHY custom park position stored from current absolute encoder values.");
    return true;
}

bool QHYMountBase::SetDefaultPark()
{
    try
    {
        if (!mount->ClearQHYPark())
            return false;
    }
    catch (EQModError &e)
    {
        return e.DefaultHandleException(this);
    }

    LOG_INFO("QHY custom park position cleared. Park will fall back to Home.");
    return true;
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

        GoHomeSP.setState(IPS_BUSY);
        GoHomeSP.apply("QHY GoHome started.");

        bool homeStarted = false;
        try
        {
            homeStarted = mount->ExecuteQHYHome();
        }
        catch (EQModError &e)
        {
            e.DefaultHandleException(this);
        }

        if (!homeStarted)
        {
            GoHomeSP.reset();
            GoHomeSP.setState(IPS_ALERT);
            GoHomeSP.apply("QHY GoHome failed to start.");
            return true;
        }

        m_CustomHomeActive = true;
        m_CustomParkActive = false;
        suppressNextGotoTracking = false;
        gotoparams.completed     = true;
        TrackState = SCOPE_SLEWING;
        RememberTrackState = TrackState;
        LOG_INFO("QHY GoHome requested using firmware-stored absolute encoder targets.");
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
