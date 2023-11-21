/*
  Copyright 2023 Northern.tech AS

  This file is part of CFEngine 3 - written and maintained by Northern.tech AS.

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; version 3.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/


#ifndef CF_MONITORING_H
#define CF_MONITORING_H


#include <cf3.defs.h>


int NovaRegisterSlot(const char *name, const char *description,
                     const char *units, double expected_minimum,
                     double expected_maximum, bool consolidable);
void NovaNamedEvent(const char *eventname, double value);
void GetObservable(int i, char *name, size_t name_size, char *desc, size_t desc_size);
void SetMeasurementPromises(Item ** classlist);
void LoadSlowlyVaryingObservations(EvalContext *ctx);
void MakeTimekey(time_t time, char *result);


#endif
