//
//  Z80.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 25/04/2021.
//  Copyright © 2021 Thomas Harte. All rights reserved.
//

#include "Z80.hpp"

#include "../FileHolder.hpp"

#include "../../Analyser/Static/ZXSpectrum/Target.hpp"
#include "../../Machines/Sinclair/ZXSpectrum/State.hpp"

using namespace Storage::State;

namespace {

std::vector<uint8_t> read_memory(Storage::FileHolder &file, size_t size, bool is_compressed) {
	if(!is_compressed) {
		return file.read(size);
	}

	std::vector<uint8_t> result(size);
	size_t cursor = 0;

	uint8_t incoming[2] = { file.get8(), file.get8()};
	while(true) {
		if(incoming[0] == 0xed && incoming[1] == 0xed) {
			const uint8_t count = file.get8();
			const uint8_t value = file.get8();

			memset(&result[cursor], value, count);
			cursor += count;
			if(cursor == size) break;
			incoming[0] = file.get8();
			incoming[1] = file.get8();
		} else {
			result[cursor] = incoming[0];
			++cursor;
			if(cursor == size) break;
			incoming[0] = incoming[1];
			incoming[1] = file.get8();
		}
	}

	return result;
}

}

std::unique_ptr<Analyser::Static::Target> Z80::load(const std::string &file_name) {
	FileHolder file(file_name);

	// Construct a target with a Spectrum state.
	using Target = Analyser::Static::ZXSpectrum::Target;
	auto result = std::make_unique<Target>();
	auto *const state = new Sinclair::ZXSpectrum::State();
	result->state = std::unique_ptr<Reflection::Struct>(state);

	// Read version 1 header.
	state->z80.registers.a = file.get8();
	state->z80.registers.flags = file.get8();
	state->z80.registers.bc = file.get16le();
	state->z80.registers.hl = file.get16le();
	state->z80.registers.program_counter = file.get16le();
	state->z80.registers.stack_pointer = file.get16le();
	state->z80.registers.ir = file.get16be();	// Stored I then R.

	// Bit 7 of R is stored separately; likely this relates to an
	// optimisation in the Z80 emulator that for some reason was
	// exported into its file format.
	const uint8_t raw_misc = file.get8();
	const uint8_t misc = (raw_misc == 0xff) ? 1 : raw_misc;
	state->z80.registers.ir = uint16_t((state->z80.registers.ir & ~0x80) | ((misc&1) << 7));

	state->z80.registers.de = file.get16le();
	state->z80.registers.bc_dash = file.get16le();
	state->z80.registers.de_dash = file.get16le();
	state->z80.registers.hl_dash = file.get16le();
	state->z80.registers.af_dash = file.get16be();	// Stored A' then F'.
	state->z80.registers.iy = file.get16le();
	state->z80.registers.ix = file.get16le();
	state->z80.registers.iff1 = bool(file.get8());
	state->z80.registers.iff2 = bool(file.get8());

	// Ignored from the next byte:
	//
	//	bit 2 = 1 	=> issue 2 emulation
	//	bit 3 = 1	=> double interrupt frequency (?)
	//	bit 4–5		=> video synchronisation (to do with emulation hackery?)
	//	bit 6–7		=> joystick type
	state->z80.registers.interrupt_mode = file.get8() & 3;

	// If the program counter is non-0 then this is a version 1 snapshot,
	// which means it's definitely a 48k image.
	if(state->z80.registers.program_counter) {
		result->model = Target::Model::FortyEightK;
		state->ram = read_memory(file, 48*1024, misc & 0x20);
		return result;
	}

	// This was a version 1 or 2 snapshot, so keep going...
	const uint16_t bonus_header_size = file.get16le();
	if(bonus_header_size != 23 && bonus_header_size != 54 && bonus_header_size != 55) {
		return nullptr;
	}

	state->z80.registers.program_counter = file.get16le();
	switch(file.get8()) {
		default: return nullptr;
		case 0:		result->model = Target::Model::FortyEightK;		break;
		case 3:		result->model = Target::Model::OneTwoEightK;	break;
		case 7:
		case 8:		result->model = Target::Model::Plus3;			break;
		case 12:	result->model = Target::Model::Plus2;			break;
		case 13:	result->model = Target::Model::Plus2a;			break;
	}

	const uint8_t last7ffd = file.get8();	(void)last7ffd;	// TODO

	file.seek(1, SEEK_CUR);
	if(file.get8() & 0x80) {
		// The 'hardware modify' bit, which inexplicably does this:
		switch(result->model) {
			default: break;
			case Target::Model::FortyEightK:	result->model = Target::Model::SixteenK;	break;
			case Target::Model::OneTwoEightK:	result->model = Target::Model::Plus2;		break;
			case Target::Model::Plus3:			result->model = Target::Model::Plus2a;		break;
		}
	}

	const uint8_t lastfffd = file.get8();	(void)lastfffd;	// TODO
	file.seek(16, SEEK_CUR);	// Sound chip registers: TODO.

	if(bonus_header_size != 23) {
		// More Z80, the emulator, lack of encapsulation to deal with here.
		const uint16_t low_t_state = file.get16le();
		const uint16_t high_t_state = file.get8();
		int time_since_interrupt;
		switch(result->model) {
			case Target::Model::SixteenK:
			case Target::Model::FortyEightK:
				time_since_interrupt = (17471 - low_t_state) + (high_t_state * 17472);
			break;

			default:
				time_since_interrupt = (17726 - low_t_state) + (high_t_state * 17727);
			break;
		}
		// TODO: map time_since_interrupt to time_into_frame, somehow.

		// Skip: Spectator flag, MGT, Multiface and other ROM flags.
		file.seek(5, SEEK_CUR);

		// Skip: highly Z80-the-emulator-specific stuff about user-defined joystick.
		file.seek(20, SEEK_CUR);

		// Skip: Disciple/Plus D stuff.
		file.seek(3, SEEK_CUR);

		if(bonus_header_size == 55) {
			const uint8_t last1ffd = file.get8();	(void)last1ffd;	// TODO
		}
	}

	// Grab RAM.
	state->ram.resize(128 * 1024);
	while(true) {
		const uint16_t block_size = file.get16le();
		const uint8_t page = file.get8();
		const auto location = file.tell();
		if(file.eof()) break;

		switch(page) {
			default: break;
		}

		file.seek(location + block_size, SEEK_SET);
	}

	return result;
}
