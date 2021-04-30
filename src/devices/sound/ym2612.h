// license:BSD-3-Clause
// copyright-holders:Aaron Giles

#ifndef MAME_SOUND_YM2612_H
#define MAME_SOUND_YM2612_H

#pragma once

#include "ymfm_mame.h"
#include "ymfm_opn.h"


// ======================> ym2612_device

DECLARE_DEVICE_TYPE(YM2612, ym2612_device);

class ym2612_device : public ymfm_device_base<ymfm::ym2612>
{
public:
	// constructor
	ym2612_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);

	// configuration helpers
	auto irq_handler() { return update_irq_handler(); }
};


// ======================> ym3438_device

DECLARE_DEVICE_TYPE(YM3438, ym3438_device);

class ym3438_device : public ymfm_device_base<ymfm::ym3438>
{
public:
	// constructor
	ym3438_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);

	// configuration helpers
	auto irq_handler() { return update_irq_handler(); }
};


// ======================> ymf276_device

DECLARE_DEVICE_TYPE(YMF276, ymf276_device);

class ymf276_device : public ymfm_device_base<ymfm::ymf276>
{
public:
	// constructor
	ymf276_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);

	// configuration helpers
	auto irq_handler() { return update_irq_handler(); }
};


#endif // MAME_SOUND_YM2612_H
