//
//  68000.hpp
//  Clock Signal
//
//  Created by Thomas Harte on 08/03/2019.
//  Copyright © 2019 Thomas Harte. All rights reserved.
//

#ifndef MC68000_h
#define MC68000_h

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <ostream>
#include <vector>

#include "../../ClockReceiver/ForceInline.hpp"
#include "../../ClockReceiver/ClockReceiver.hpp"
#include "../RegisterSizes.hpp"

namespace CPU {
namespace MC68000 {

/*!
	A microcycle is an atomic unit of 68000 bus activity — it is a single item large enough
	fully to specify a sequence of bus events that occur without any possible interruption.

	Concretely, a standard read cycle breaks down into at least two microcycles:

		1) a 4 half-cycle length microcycle in which the address strobe is signalled; and
		2) a 4 half-cycle length microcycle in which at least one of the data strobes is
		signalled, and the data bus is sampled.

	That is, assuming DTack were signalled when microcycle (1) ended. If not then additional
	wait state microcycles would fall between those two parts.

	The 68000 data sheet defines when the address becomes valid during microcycle (1), and
	when the address strobe is actually asserted. But those timings are fixed. So simply
	telling you that this was a microcycle during which the address trobe was signalled is
	sufficient fully to describe the bus activity.

	(Aside: see the 68000 template's definition for options re: implicit DTack; if your
	68000 owner can always predict exactly how long it will hold DTack following observation
	of an address-strobing microcycle, it can just supply those periods for accounting and
	avoid the runtime cost of actual DTack emulation. But such as the bus allows.)
*/
struct Microcycle {
	using OperationT = unsigned int;

	/// Indicates that the address strobe and exactly one of the data strobes are active; you can determine
	/// which by inspecting the low bit of the provided address. The RW line indicates a read.
	static constexpr OperationT SelectByte				= 1 << 0;
	// Maintenance note: this is bit 0 to reduce the cost of getting a host-endian
	// bytewise address. The assumption that it is bit 0 is also used for branchless
	// selection in a few places. See implementation of host_endian_byte_address(),
	// value8_high(), value8_low() and value16().

	/// Indicates that the address and both data select strobes are active.
	static constexpr OperationT SelectWord				= 1 << 1;

	/// If set, indicates a read. Otherwise, a write.
	static constexpr OperationT Read 					= 1 << 2;

	// Two-bit gap deliberately left here for PermitRead/Write below.

	/// A NewAddress cycle is one in which the address strobe is initially low but becomes high;
	/// this correlates to states 0 to 5 of a standard read/write cycle.
	static constexpr OperationT NewAddress				= 1 << 5;

	/// A SameAddress cycle is one in which the address strobe is continuously asserted, but neither
	/// of the data strobes are.
	static constexpr OperationT SameAddress				= 1 << 6;

	/// A Reset cycle is one in which the RESET output is asserted.
	static constexpr OperationT Reset					= 1 << 7;

	/// Contains the value of line FC0 if it is not implicit via InterruptAcknowledge.
	static constexpr OperationT IsData 					= 1 << 8;

	/// Contains the value of line FC1 if it is not implicit via InterruptAcknowledge.
	static constexpr OperationT IsProgram 				= 1 << 9;

	/// The interrupt acknowledge cycle is that during which the 68000 seeks to obtain the vector for
	/// an interrupt it plans to observe. Noted on a real 68000 by all FCs being set to 1.
	static constexpr OperationT InterruptAcknowledge	= 1 << 10;

	/// Represents the state of the 68000's valid memory address line — indicating whether this microcycle
	/// is synchronised with the E clock to satisfy a valid peripheral address request.
	static constexpr OperationT IsPeripheral 			= 1 << 11;

	/// Provides the 68000's bus grant line — indicating whether a bus request has been acknowledged.
	static constexpr OperationT BusGrant				= 1 << 12;

	/// Contains a valid combination of the various static constexpr int flags, describing the operation
	/// performed by this Microcycle.
	OperationT operation = 0;

	/// Describes the duration of this Microcycle.
	HalfCycles length = HalfCycles(4);

	/*!
		For expediency, this provides a full 32-bit byte-resolution address — e.g.
		if reading indirectly via an address register, this will indicate the full
		value of the address register.

		The receiver should ignore bits 0 and 24+. Use word_address() automatically
		to obtain the only the 68000's real address lines, giving a 23-bit address
		at word resolution.
	*/
	const uint32_t *address = nullptr;

	/*!
		If this is a write cycle, dereference value to get the value loaded onto
		the data bus.

		If this is a read cycle, write the value on the data bus to it.

		Otherwise, this value is undefined.

		Byte values are provided via @c value.halves.low. @c value.halves.high is undefined.
		This is true regardless of whether the upper or lower byte of a word is being
		accessed.

		Word values occupy the entirety of @c value.full.
	*/
	RegisterPair16 *value = nullptr;

	/// @returns @c true if two Microcycles are equal; @c false otherwise.
	bool operator ==(const Microcycle &rhs) const {
		if(value != rhs.value) return false;
		if(address != rhs.address) return false;
		if(length != rhs.length) return false;
		if(operation != rhs.operation) return false;
		return true;
	}

	// Various inspectors.

	/*! @returns true if any data select line is active; @c false otherwise. */
	forceinline bool data_select_active() const {
		return bool(operation & (SelectWord | SelectByte | InterruptAcknowledge));
	}

	/*!
		@returns 0 if this byte access wants the low part of a 16-bit word; 8 if it wants the high part.
	*/
	forceinline unsigned int byte_shift() const {
		return (((*address) & 1) << 3) ^ 8;
	}

	/*!
		Obtains the mask to apply to a word that will leave only the byte this microcycle is selecting.

		@returns 0x00ff if this byte access wants the low part of a 16-bit word; 0xff00 if it wants the high part.
	*/
	forceinline uint16_t byte_mask() const {
		return uint16_t(0xff00) >> (((*address) & 1) << 3);
	}

	/*!
		Obtains the mask to apply to a word that will leave only the byte this microcycle **isn't** selecting.
		i.e. this is the part of a word that should be untouched by this microcycle.

		@returns 0xff00 if this byte access wants the low part of a 16-bit word; 0x00ff if it wants the high part.
	*/
	forceinline uint16_t untouched_byte_mask() const {
		return uint16_t(uint16_t(0xff) << (((*address) & 1) << 3));
	}

	/*!
		Assuming this cycle is a byte write, mutates @c destination by writing the byte to the proper upper or
		lower part, retaining the other half.
	*/
	forceinline uint16_t write_byte(uint16_t destination) const {
		return uint16_t((destination & untouched_byte_mask()) | (value->halves.low << byte_shift()));
	}

	/*!
		@returns non-zero if this is a byte read and 68000 LDS is asserted.
	*/
	forceinline int lower_data_select() const {
		return (operation & SelectByte) & ((*address & 1) << 3);
	}

	/*!
		@returns non-zero if this is a byte read and 68000 UDS is asserted.
	*/
	forceinline int upper_data_select() const {
		return (operation & SelectByte) & ~((*address & 1) << 3);
	}

	/*!
		@returns the address being accessed at the precision a 68000 supplies it —
		only 24 address bit precision, with the low bit shifted out. So it's the
		68000 address at word precision: address 0 is the first word in the address
		space, address 1 is the second word (i.e. the third and fourth bytes) in
		the address space, etc.
	*/
	forceinline uint32_t word_address() const {
		return (address ? (*address) & 0x00fffffe : 0) >> 1;
	}

	/*!
		@returns the address of the word or byte being accessed at byte precision,
		in the endianness of the host platform.

		So: if this is a word access, and the 68000 wants to select the word at address
		@c n, this will evaluate to @c n regardless of the host machine's endianness..

		If this is a byte access and the host machine is big endian it will evalue to @c n.

		If the host machine is little endian then it will evaluate to @c n^1.
	*/
	forceinline uint32_t host_endian_byte_address() const {
		#if TARGET_RT_BIG_ENDIAN
		return *address & 0xffffff;
		#else
		return (*address ^ (1 & operation & SelectByte)) & 0xffffff;
		#endif
	}

	/*!
		@returns the value on the data bus — all 16 bits, with any inactive lines
		(as er the upper and lower data selects) being represented by 1s. Assumes
		this is a write cycle.
	*/
	forceinline uint16_t value16() const {
		const uint16_t values[] = { value->full, uint16_t((value->halves.low << 8) | value->halves.low) };
		return values[operation & SelectByte];
	}

	/*!
		@returns the value currently on the high 8 lines of the data bus if any;
		@c 0xff otherwise. Assumes this is a write cycle.
	*/
	forceinline uint8_t value8_high() const {
		const uint8_t values[] = { uint8_t(value->full >> 8), value->halves.low};
		return values[operation & SelectByte];
	}

	/*!
		@returns the value currently on the low 8 lines of the data bus if any;
		@c 0xff otherwise. Assumes this is a write cycle.
	*/
	forceinline uint8_t value8_low() const {
		const uint8_t values[] = { uint8_t(value->full), value->halves.low};
		return values[operation & SelectByte];
	}

	/*!
		Sets to @c value the 8- or 16-bit portion of the supplied value that is
		currently being read. Assumes this is a read cycle.
	*/
	forceinline void set_value16(uint16_t v) const {
		assert(operation & Microcycle::Read);
		if(operation & Microcycle::SelectWord) {
			value->full = v;
		} else {
			value->halves.low = uint8_t(v >> byte_shift());
		}
	}

	/*!
		Equivalent to set_value16((v << 8) | 0x00ff).
	*/
	forceinline void set_value8_high(uint8_t v) const {
		assert(operation & Microcycle::Read);
		if(operation & Microcycle::SelectWord) {
			value->full = uint16_t(0x00ff | (v << 8));
		} else {
			value->halves.low = uint8_t(v | (0xff00 >> ((*address & 1) << 3)));
		}
	}

	/*!
		Equivalent to set_value16((v) | 0xff00).
	*/
	forceinline void set_value8_low(uint8_t v) const {
		assert(operation & Microcycle::Read);
		if(operation & Microcycle::SelectWord) {
			value->full = 0xff00 | v;
		} else {
			value->halves.low = uint8_t(v | (0x00ff << ((*address & 1) << 3)));
		}
	}

	/*!
		@returns the same value as word_address() for any Microcycle with the NewAddress or
		SameAddress flags set; undefined behaviour otherwise.
	*/
	forceinline uint32_t active_operation_word_address() const {
		return ((*address) & 0x00fffffe) >> 1;
	}

	// PermitRead and PermitWrite are used as part of the read/write mask
	// supplied to @c apply; they are picked to be small enough values that
	// a byte can be used for storage.
	static constexpr OperationT PermitRead 	= 1 << 3;
	static constexpr OperationT PermitWrite	= 1 << 4;

	/*!
		Assuming this to be a cycle with a data select active, applies it to @c target
		subject to the read_write_mask, where 'applies' means:

			* if this is a byte read, reads a single byte from @c target;
			* if this is a word read, reads a word (in the host platform's endianness) from @c target; and
			* if this is a write, does the converse of a read.
	*/
	forceinline void apply(uint8_t *target, OperationT read_write_mask = PermitRead | PermitWrite) const {
		assert( (operation & (SelectWord | SelectByte)) != (SelectWord | SelectByte));

		switch((operation | read_write_mask) & (SelectWord | SelectByte | Read | PermitRead | PermitWrite)) {
			default:
			break;

			case SelectWord | Read | PermitRead:
			case SelectWord | Read | PermitRead | PermitWrite:
				value->full = *reinterpret_cast<uint16_t *>(target);
			break;
			case SelectByte | Read | PermitRead:
			case SelectByte | Read | PermitRead | PermitWrite:
				value->halves.low = *target;
			break;
			case SelectWord | PermitWrite:
			case SelectWord | PermitWrite | PermitRead:
				*reinterpret_cast<uint16_t *>(target) = value->full;
			break;
			case SelectByte | PermitWrite:
			case SelectByte | PermitWrite | PermitRead:
				*target = value->halves.low;
			break;
		}
	}

#ifndef NDEBUG
	bool is_resizeable = false;
#endif
};

/*!
	This is the prototype for a 68000 bus handler; real bus handlers can descend from this
	in order to get default implementations of any changes that may occur in the expected interface.
*/
class BusHandler {
	public:
		/*!
			Provides the bus handler with a single Microcycle to 'perform'.

			FC0 and FC1 are provided inside the microcycle as the IsData and IsProgram
			flags; FC2 is provided here as is_supervisor — it'll be either 0 or 1.
		*/
		HalfCycles perform_bus_operation([[maybe_unused]] const Microcycle &cycle, [[maybe_unused]] int is_supervisor) {
			return HalfCycles(0);
		}

		void flush() {}

		/*!
			Provides information about the path of execution if enabled via the template.
		*/
		void will_perform([[maybe_unused]] uint32_t address, [[maybe_unused]] uint16_t opcode) {}
};

#include "Implementation/68000Storage.hpp"

class ProcessorBase: public ProcessorStorage {
};

enum Flag: uint16_t {
	Trace		= 0x8000,
	Supervisor	= 0x2000,

	ConditionCodes	= 0x1f,

	Extend		= 0x0010,
	Negative	= 0x0008,
	Zero		= 0x0004,
	Overflow	= 0x0002,
	Carry		= 0x0001
};

struct ProcessorState {
	uint32_t data[8];
	uint32_t address[7];
	uint32_t user_stack_pointer, supervisor_stack_pointer;
	uint32_t program_counter;
	uint16_t status;

	/*!
		@returns the supervisor stack pointer if @c status indicates that
			the processor is in supervisor mode; the user stack pointer otherwise.
	*/
	uint32_t stack_pointer() const {
		return (status & Flag::Supervisor) ? supervisor_stack_pointer : user_stack_pointer;
	}

	// TODO: More state needed to indicate current instruction, the processor's
	// progress through it, and anything it has fetched so far.
//			uint16_t current_instruction;
};

template <class T, bool dtack_is_implicit, bool signal_will_perform = false> class Processor: public ProcessorBase {
	public:
		Processor(T &bus_handler) : ProcessorBase(), bus_handler_(bus_handler) {}

		void run_for(HalfCycles duration);

		using State = ProcessorState;
		/// @returns The current processor state.
		State get_state();

		/// Sets the processor to the supplied state.
		void set_state(const State &);

		/// Sets the DTack line — @c true for active, @c false for inactive.
		inline void set_dtack(bool dtack) {
			dtack_ = dtack;
		}

		/// Sets the VPA (valid peripheral address) line — @c true for active, @c false for inactive.
		inline void set_is_peripheral_address(bool is_peripheral_address) {
			is_peripheral_address_ = is_peripheral_address;
		}

		/// Sets the bus error line — @c true for active, @c false for inactive.
		inline void set_bus_error(bool bus_error) {
			bus_error_ = bus_error;
		}

		/// Sets the interrupt lines, IPL0, IPL1 and IPL2.
		inline void set_interrupt_level(int interrupt_level) {
			bus_interrupt_level_ = interrupt_level;
		}

		/// Sets the bus request line.
		/// This area of functionality is TODO.
		inline void set_bus_request(bool bus_request) {
			bus_request_ = bus_request;
		}

		/// Sets the bus acknowledge line.
		/// This area of functionality is TODO.
		inline void set_bus_acknowledge(bool bus_acknowledge) {
			bus_acknowledge_ = bus_acknowledge;
		}

		/// Sets the halt line.
		inline void set_halt(bool halt) {
			halt_ = halt;
		}

		/// @returns The current phase of the E clock; this will be a number of
		/// half-cycles between 0 and 19 inclusive, indicating how far the 68000
		/// is into the current E cycle.
		///
		/// This is guaranteed to be 0 at initial 68000 construction.
		HalfCycles get_e_clock_phase() {
			return e_clock_phase_;
		}

	private:
		T &bus_handler_;
};

#include "Implementation/68000Implementation.hpp"

}
}

#endif /* MC68000_h */
