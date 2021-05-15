// license:BSD-3-Clause
// copyright-holders:hap
/***************************************************************************

    Saitek OSA Expansion Slot modules

***************************************************************************/

#include "emu.h"
#include "modules.h"

#include "maestroa.h"

void saitekosa_expansion_modules(device_slot_interface &device)
{
	device.option_add("maestroa", OSA_MAESTROA);
}
