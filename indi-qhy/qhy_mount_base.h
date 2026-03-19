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

#pragma once

#include "eqmod/eqmodbase.h"

class QHYMountBase : public EQMod
{
    public:
        QHYMountBase();
    protected:
        virtual const char *getDefaultName() override;
        virtual bool initProperties() override;
        virtual bool updateProperties() override;
        virtual bool ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int n) override;
        virtual bool updateTime(ln_date *utc, double utc_offset) override;
        virtual bool updateLocation(double latitude, double longitude, double elevation) override;

    private:
        INDI::PropertySwitch GoHomeSP {1};
};
