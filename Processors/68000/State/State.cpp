//
//  State.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 14/05/2020.
//  Copyright © 2020 Thomas Harte. All rights reserved.
//

#include "State.hpp"

using namespace CPU::MC68000;

State::State(const ProcessorBase &src): State() {
	// Registers.
	for(int c = 0; c < 7; ++c) {
		registers.address[c] = src.address_[c].full;
		registers.data[c] = src.data_[c].full;
	}
	registers.data[7] = src.data_[7].full;
	registers.user_stack_pointer = src.is_supervisor_ ? src.stack_pointers_[0].full : src.address_[7].full;
	registers.supervisor_stack_pointer = src.is_supervisor_ ? src.address_[7].full : src.stack_pointers_[1].full;
	registers.status = src.get_status();
	registers.program_counter = src.program_counter_.full;
	registers.prefetch = src.prefetch_queue_.full;
	registers.instruction = src.decoded_instruction_.full;

	// Inputs.
	inputs.bus_interrupt_level = uint8_t(src.bus_interrupt_level_);
	inputs.dtack = src.dtack_;
	inputs.is_peripheral_address = src.is_peripheral_address_;
	inputs.bus_error = src.bus_error_;
	inputs.bus_request = src.bus_request_;
	inputs.bus_grant = false;	// TODO (within the 68000).
	inputs.halt = src.halt_;

	// TODO:
	//	execution_state_
	//	active_[program_/micro_op_/step_]

	// Execution state.
	execution_state.e_clock_phase = src.e_clock_phase_.as<uint8_t>();
	execution_state.effective_address[0] = src.effective_address_[0].full;
	execution_state.effective_address[1] = src.effective_address_[1].full;
	execution_state.source_data = src.source_bus_data_[0].full;
	execution_state.destination_data = src.destination_bus_data_[0].full;
	execution_state.last_trace_flag = src.last_trace_flag_;
	execution_state.next_word = src.next_word_;
	execution_state.dbcc_false_address = src.dbcc_false_address_;
	execution_state.is_starting_interrupt = src.is_starting_interrupt_;
	execution_state.pending_interrupt_level = uint8_t(src.pending_interrupt_level_);
	execution_state.accepted_interrupt_level = uint8_t(src.accepted_interrupt_level_);
}

void State::apply(ProcessorBase &target) {
}

// Boilerplate follows here, to establish 'reflection'.
State::State() {
	if(needs_declare()) {
		DeclareField(registers);
		DeclareField(execution_state);
		DeclareField(inputs);
	}
}

State::Registers::Registers() {
	if(needs_declare()) {
		DeclareField(data);
		DeclareField(address);
		DeclareField(user_stack_pointer);
		DeclareField(supervisor_stack_pointer);
		DeclareField(status);
		DeclareField(program_counter);
		DeclareField(prefetch);
		DeclareField(instruction);
	}
}

State::Inputs::Inputs() {
	if(needs_declare()) {
		DeclareField(bus_interrupt_level);
		DeclareField(dtack);
		DeclareField(is_peripheral_address);
		DeclareField(bus_error);
		DeclareField(bus_request);
		DeclareField(bus_grant);
		DeclareField(halt);
	}
}

State::ExecutionState::ExecutionState() {
	if(needs_declare()) {
		DeclareField(e_clock_phase);
		DeclareField(effective_address);
		DeclareField(source_data);
		DeclareField(destination_data);
		DeclareField(last_trace_flag);
		DeclareField(next_word);
		DeclareField(dbcc_false_address);
		DeclareField(is_starting_interrupt);
		DeclareField(pending_interrupt_level);
		DeclareField(accepted_interrupt_level);
	}
}
