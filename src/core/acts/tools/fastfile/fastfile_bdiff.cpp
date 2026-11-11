#include <includes.hpp>
#include <tools/fastfile/fastfile_bdiff.hpp>
#include <xxhash.h>
#include <acts_api_impl/api_impl.hpp>
#include <hook/error.hpp>
#include <atomic>
#include <thread>

namespace fastfile::bdiff {
    namespace {
        struct VcdState {
            unsigned int next_slot;
            size_t anear[4];
            size_t asame[768];
            unsigned char* pAddr;
        };

        enum VCD { VCD_SELF = 0, VCD_HERE = 1 };

        enum VCDInstructionCode : uint16_t { VCDI_NOOP = 0, VCDI_ADD = 1, VCDI_RUN = 2, VCDI_COPY = 3, VCDI_COUNT };
        static const char* vcdInstructionCodeNames[VCDI_COUNT]{ "NOOP", "ADD", "RUN", "COPY" };

        struct Instruction {
            VCDInstructionCode op;
            uint16_t mode;
            uint32_t size;
        };

        struct DoubleInstruction {
            Instruction instruction[2];
        };
        static DoubleInstruction instructions[256] = { { { { VCDI_RUN, 0x0, 0x0 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0x0 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0x5 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0x6 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0x7 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0x8 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0x9 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0xa }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0xb }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0xc }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0xd }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0xe }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0xf }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0x10 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0x11 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x0, 0x0 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x0, 0x4 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x0, 0x5 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x0, 0x6 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x0, 0x7 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x0, 0x8 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x0, 0x9 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x0, 0xa }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x0, 0xb }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x0, 0xc }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x0, 0xd }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x0, 0xe }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x0, 0xf }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x0, 0x10 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x0, 0x11 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x0, 0x12 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x1, 0x0 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x1, 0x4 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x1, 0x5 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x1, 0x6 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x1, 0x7 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x1, 0x8 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x1, 0x9 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x1, 0xa }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x1, 0xb }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x1, 0xc }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x1, 0xd }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x1, 0xe }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x1, 0xf }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x1, 0x10 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x1, 0x11 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x1, 0x12 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x2, 0x0 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x2, 0x4 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x2, 0x5 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x2, 0x6 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x2, 0x7 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x2, 0x8 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x2, 0x9 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x2, 0xa }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x2, 0xb }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x2, 0xc }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x2, 0xd }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x2, 0xe }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x2, 0xf }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x2, 0x10 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x2, 0x11 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x2, 0x12 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x3, 0x0 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x3, 0x4 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x3, 0x5 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x3, 0x6 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x3, 0x7 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x3, 0x8 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x3, 0x9 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x3, 0xa }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x3, 0xb }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x3, 0xc }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x3, 0xd }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x3, 0xe }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x3, 0xf }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x3, 0x10 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x3, 0x11 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x3, 0x12 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x4, 0x0 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x4, 0x4 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x4, 0x5 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x4, 0x6 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x4, 0x7 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x4, 0x8 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x4, 0x9 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x4, 0xa }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x4, 0xb }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x4, 0xc }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x4, 0xd }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x4, 0xe }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x4, 0xf }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x4, 0x10 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x4, 0x11 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x4, 0x12 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x5, 0x0 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x5, 0x4 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x5, 0x5 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x5, 0x6 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x5, 0x7 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x5, 0x8 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x5, 0x9 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x5, 0xa }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x5, 0xb }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x5, 0xc }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x5, 0xd }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x5, 0xe }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x5, 0xf }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x5, 0x10 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x5, 0x11 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x5, 0x12 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x6, 0x0 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x6, 0x4 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x6, 0x5 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x6, 0x6 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x6, 0x7 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x6, 0x8 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x6, 0x9 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x6, 0xa }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x6, 0xb }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x6, 0xc }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x6, 0xd }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x6, 0xe }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x6, 0xf }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x6, 0x10 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x6, 0x11 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x6, 0x12 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x7, 0x0 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x7, 0x4 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x7, 0x5 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x7, 0x6 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x7, 0x7 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x7, 0x8 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x7, 0x9 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x7, 0xa }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x7, 0xb }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x7, 0xc }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x7, 0xd }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x7, 0xe }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x7, 0xf }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x7, 0x10 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x7, 0x11 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x7, 0x12 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x8, 0x0 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x8, 0x4 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x8, 0x5 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x8, 0x6 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x8, 0x7 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x8, 0x8 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x8, 0x9 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x8, 0xa }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x8, 0xb }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x8, 0xc }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x8, 0xd }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x8, 0xe }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x8, 0xf }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x8, 0x10 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x8, 0x11 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_COPY, 0x8, 0x12 }, { VCDI_NOOP, 0x0, 0x0 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x0, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x0, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x0, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x0, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x0, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x0, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x0, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x0, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x0, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x0, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x0, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x0, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x1, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x1, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x1, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x1, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x1, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x1, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x1, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x1, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x1, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x1, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x1, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x1, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x2, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x2, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x2, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x2, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x2, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x2, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x2, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x2, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x2, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x2, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x2, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x2, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x3, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x3, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x3, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x3, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x3, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x3, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x3, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x3, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x3, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x3, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x3, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x3, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x4, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x4, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x4, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x4, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x4, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x4, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x4, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x4, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x4, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x4, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x4, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x4, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x5, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x5, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x5, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x5, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x5, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x5, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x5, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x5, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x5, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x5, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x5, 0x5 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x5, 0x6 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x6, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x6, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x6, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x6, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x7, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x7, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x7, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x7, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x1 }, { VCDI_COPY, 0x8, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x2 }, { VCDI_COPY, 0x8, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x3 }, { VCDI_COPY, 0x8, 0x4 } } },
                                                       { { { VCDI_ADD, 0x0, 0x4 }, { VCDI_COPY, 0x8, 0x4 } } },
                                                       { { { VCDI_COPY, 0x0, 0x4 }, { VCDI_ADD, 0x0, 0x1 } } },
                                                       { { { VCDI_COPY, 0x1, 0x4 }, { VCDI_ADD, 0x0, 0x1 } } },
                                                       { { { VCDI_COPY, 0x2, 0x4 }, { VCDI_ADD, 0x0, 0x1 } } },
                                                       { { { VCDI_COPY, 0x3, 0x4 }, { VCDI_ADD, 0x0, 0x1 } } },
                                                       { { { VCDI_COPY, 0x4, 0x4 }, { VCDI_ADD, 0x0, 0x1 } } },
                                                       { { { VCDI_COPY, 0x5, 0x4 }, { VCDI_ADD, 0x0, 0x1 } } },
                                                       { { { VCDI_COPY, 0x6, 0x4 }, { VCDI_ADD, 0x0, 0x1 } } },
                                                       { { { VCDI_COPY, 0x7, 0x4 }, { VCDI_ADD, 0x0, 0x1 } } },
                                                       { { { VCDI_COPY, 0x8, 0x4 }, { VCDI_ADD, 0x0, 0x1 } } } };

        enum VCDFlags {
            VCDF_UNK1 = 1,
            VCDF_UNK2 = 2,
            VCDF_MASK12 = VCDF_UNK2 | VCDF_UNK1,
            VCDF_SOURCE_SEGMENT_RELATIVE = 4,
            VCDF_CHECKSUM = 8,
        };

        size_t ReadULEB128(byte** data) {
            size_t output{};
            byte c;

            do {
                c = **data;
                (*data)++;
                output = (output << 7) | (c & 0x7F);
            } while (c & 0x80);

            return output;
        }

        size_t AddressDecode(VcdState* vcd, size_t here, int mode) {
            size_t address;

            if (mode == VCD_SELF) {
                address = ReadULEB128(&vcd->pAddr);
            } else if (mode == VCD_HERE) {
                address = here - ReadULEB128(&vcd->pAddr);
            } else if (mode >= 6) {
                int index = (mode - 6) << 8;
                index += *(vcd->pAddr++);
                address = vcd->asame[index];
            } else {
                address = vcd->anear[mode - 2] + ReadULEB128(&vcd->pAddr);
            }

            vcd->anear[vcd->next_slot] = address;
            vcd->next_slot = (vcd->next_slot + 1) & 3;
            vcd->asame[address % 0x300] = address;

            return address;
        }

        void memmove2(void* _dest, void* _src, size_t n) {
            byte* dest{ (byte*)_dest };
            byte* src{ (byte*)_src };

            if (&dest[n] <= src || &src[n] <= dest) {
                std::memcpy(dest, src, n);
                return; // no overlap
            }

            for (size_t i = 0; i < n; i++) {
                dest[i] = src[i];
            }
        }
    } // namespace

    bool bdiff(BDiffState* state, vcSourceCB_t* sourceDataCB, vcDiffCB_t* patchDataCB, vcDestCB_t* destDataCB) {
        if (!state->headerRead) {
            byte* header{ patchDataCB(state->state, 0, 0x405, nullptr) };

            if (!header || header[0] != 0xD6 || header[1] != 0xC3 || header[2] != 0xC4) {
                state->error = "Invalid magic";
                return false;
            }
            if ((header[4] & 0xF3) != 0) {
                state->error = "Invalid header";
                return false;
            }
            patchDataCB(state->state, 5, 0, nullptr);
            state->headerRead = true;
        }

        size_t offset{};
        // fetch enough data for the block info, 0x400 is the alignment?
        byte* bdiffData{ patchDataCB(state->state, 0, 0x400, &offset) };
        if (!bdiffData) {
            state->error = "Missing data";
            return false;
        }
        byte* bdiffDataStart{ bdiffData };
        byte flags{ *(bdiffData++) };
        if ((flags & VCDF_MASK12) == VCDF_MASK12) {
            state->error = "Invalid flag";
            return false;
        }
        size_t sourceSegmentOffset{};
        byte* sourceSegment{};
        if (flags & VCDF_MASK12) {
            sourceSegmentOffset = ReadULEB128(&bdiffData);
            size_t sourceSegmentLocationEx{ ReadULEB128(&bdiffData) };
            size_t sourceSegmentLocation;
            if (flags & VCDF_SOURCE_SEGMENT_RELATIVE) {
                sourceSegmentLocation = ReadULEB128(&bdiffData);
            } else {
                sourceSegmentLocation = sourceSegmentLocationEx;
            }

            size_t sourceSegmentStart{ sourceSegmentLocationEx - sourceSegmentLocation };
            sourceSegment = &sourceDataCB(
                state->state,
                sourceSegmentLocation,
                sourceSegmentStart + sourceSegmentOffset
            )[sourceSegmentStart];
        }

        size_t patchDataLen{ ReadULEB128(&bdiffData) };

        byte* patchData{ patchDataCB(
            state->state,
            offset + bdiffData - bdiffDataStart,
            patchDataLen + (flags & VCDF_CHECKSUM ? 4 : 0),
            &offset
        ) };
        byte* patchDataStart{ patchData };

        size_t patchDestLen{ ReadULEB128(&patchData) };
        byte* patchDest{ destDataCB(state->state, patchDestLen) };
        byte* patchDestStart{ patchDest };
        if (!patchDest) {
            state->error = "No patch data destination";
            return false;
        }

        if (*(patchData++)) {
            state->error = "Invalid patch data";
            return false;
        }

        VcdState vcd{};
        size_t instructionOffset{ ReadULEB128(&patchData) };
        size_t instructionSize{ ReadULEB128(&patchData) };
        size_t instructionCheckOffset{ ReadULEB128(&patchData) };

        byte* instructionData{ &patchData[instructionOffset] };
        byte* instructionDataEnd{ &patchData[instructionOffset + instructionSize] };
        byte* instructionCheck{ &instructionDataEnd[instructionCheckOffset] };

        vcd.pAddr = instructionDataEnd;

        // load the diff instructions
        while (instructionData < instructionDataEnd) {
            byte opcode{ *(instructionData++) };
            DoubleInstruction* dinst{ &instructions[opcode] };

            for (size_t i = 0; i < 2; i++) {
                Instruction inst{ dinst->instruction[i] };
                assert(inst.op < VCDI_COUNT && "invalid op");

                size_t ilen{ inst.size };
                if (!ilen && inst.op != VCDI_NOOP)
                    ilen = ReadULEB128(&instructionData);

                // LOG_TRACE(
                //     "r:0x{:03x} 0x{:02x} inst[{}] = "
                //     "(VCDI_{}, m:{}, s:0x{:x}/0x{:x})"
                //     " 0x{:x}/0x{:x}"
                //     " 0x{:x}",
                //     instructionDataEnd - instructionData, (int)opcode, i,
                //     vcdInstructionCodeNames[inst.op], inst.mode, inst.size, ilen,
                //     patchDest - patchDestStart, patchDestLen,
                //     patchData - patchDataStart
                //);

                switch (inst.op) {
                case VCDI_NOOP: // 0 no op
                    break;
                case VCDI_ADD: { // 1 add to block
                    std::memmove(patchDest, patchData, ilen);
                    patchData += ilen;
                    patchDest += ilen;
                    break;
                }
                case VCDI_RUN: { // 2 empty a block
                    byte runchar{ *(patchData++) };
                    std::memset(patchDest, runchar, ilen);
                    patchDest += ilen;
                    break;
                }
                case VCDI_COPY: { // 3 copy existing block
                    size_t address{ AddressDecode(&vcd, sourceSegmentOffset + patchDest - patchDestStart, inst.mode) };

                    byte* readLoc;
                    if (address < sourceSegmentOffset) {
                        if (!sourceSegment) {
                            state->error = "Missing source segment";
                            return false;
                        }
                        readLoc = &sourceSegment[address];
                        // LOG_TRACE("VCDI_COPY(0x{:x}, b:0x{:x}, 0x{:x})", patchDest - patchDestStart, address, ilen);
                    } else {
                        readLoc = &patchDestStart[address - sourceSegmentOffset];
                        // LOG_TRACE("VCDI_COPY(0x{:x}, d:0x{:x}, 0x{:x})", patchDest - patchDestStart, address -
                        // sourceSegmentOffset, ilen);
                    }

                    memmove2(patchDest, readLoc, ilen);
                    patchDest += ilen;
                    break;
                }
                }
            }
        }

        if (flags & VCDF_CHECKSUM) {
            // check checksum
            uint32_t realChecksum{ *(uint32_t*)instructionCheck };
            instructionCheck += sizeof(uint32_t);

            switch (state->type) {
            case BDT_TREYARCH: {
                uint32_t calculatedChecksum{ XXH32(patchDestStart, patchDestLen, 0) };
                if (calculatedChecksum != realChecksum) {
                    state->error = utils::va("Data is corrupt. %x != %x", realChecksum, calculatedChecksum);
                    return false;
                }
                LOG_TRACE("validated bdiff checksum {:x}", realChecksum);
                break;
            }
            case BDT_IW: {
                uint64_t xxh64{ XXH64(patchDestStart, patchDestLen, state->lastChecksum) };
                uint32_t calculatedChecksum{ (uint32_t)(xxh64 ^ (xxh64 >> 32)) };
                if (calculatedChecksum != realChecksum) {
                    state->error = utils::va(
                        "Data is corrupt. %x != %x (last=%x)",
                        realChecksum,
                        calculatedChecksum,
                        state->lastChecksum
                    );
                    return false;
                }
                LOG_TRACE("validated bdiff checksum {:x}", realChecksum);
                break;
            }
            default:
                break;
            }
            state->lastChecksum = realChecksum;
        }

        // set the offset to the next bdiff block
        patchDataCB(state->state, instructionCheck - patchDataStart + offset, 0, nullptr);
        return true;
    }

    class DiffState {
        std::vector<byte> outwindow{};
        byte tmpPatch[0x405]{};
        byte* destWindow{};
        size_t destWindowSize{};
        size_t patchWindowOffsetLast{};
        size_t destWindowLastSize{};
        core::bytebuffer::ByteBuffer* sourceData{};
        core::bytebuffer::ByteBuffer* patchData{};

        std::vector<byte>* destdata{};

      public:
        DiffState(
            size_t winsize, core::bytebuffer::ByteBuffer* sourceData, core::bytebuffer::ByteBuffer* patchData,
            std::vector<byte>* destdata
        )
            : sourceData(sourceData), patchData(patchData), destdata(destdata) {
            outwindow.resize(winsize);
            destWindow = outwindow.data();
        }

        void SyncData() {
            if (!destWindowLastSize)
                return;

            LOG_TRACE("Sync 0x{:x} bytes", destWindowLastSize);
            utils::WriteValue(*destdata, destWindow, destWindowLastSize);

            destWindowLastSize = 0;
        }

        byte* LoadSourceData(size_t offset, size_t size) {
            sourceData->Goto(offset);
            if (!sourceData->CanRead(size)) {
                hook::error::DumpStackTraceFrom();
                throw std::runtime_error(
                    std::format("vcSourceCB_t: read too much at 0x{:x}/0x{:x}", sourceData->Loc(), size)
                );
            }
            LOG_TRACE("vcSourceCB_t: read 0x{:x}:0x{:x}", sourceData->Loc(), size);
            return sourceData->ReadPtr<uint8_t>(size);
        }

        byte* LoadPatchData(size_t offset, size_t size, size_t* pOffset) {
            if (offset) {
                patchWindowOffsetLast = offset;
            } else {
                offset = patchWindowOffsetLast;
            }
            if (pOffset)
                *pOffset = offset;

            patchData->Goto(offset);
            if (!patchData->CanRead(size)) {
                if (sizeof(tmpPatch) >= size) {
                    // use the tmp patch data
                    size_t loaded{ patchData->Remaining() };
                    patchData->Read(tmpPatch, loaded);
                    std::memset(&tmpPatch[loaded], 0, sizeof(tmpPatch) - loaded);
                    return tmpPatch;
                }

                hook::error::DumpStackTraceFrom();
                throw std::runtime_error(
                    std::format("vcDiffCB_t: read too much at 0x{:x}/0x{:x}", patchData->Loc(), size)
                );
            }
            LOG_TRACE("vcDiffCB_t: read 0x{:x}:0x{:x}", patchData->Loc(), size);
            return patchData->ReadPtr<uint8_t>(size);
        }

        byte* LoadDestData(size_t size) {
            SyncData();
            destWindowLastSize = size;
            if (size > outwindow.size()) {
                // allocate what we need
                LOG_TRACE("vcDestCB_t: resize to 0x{:x}", size);
                outwindow.resize(size);
                destWindow = outwindow.data();
            }
            LOG_TRACE("vcDestCB_t: give 0x{:x}", size);
            return destWindow;
        }

        constexpr size_t GetLastSize() const { return destWindowLastSize; };
    };

    std::vector<byte> bdiff(
        core::bytebuffer::ByteBuffer* sourceData, core::bytebuffer::ByteBuffer* patchData, BDiffType type,
        size_t winsize
    ) {
        std::vector<byte> destdata{};
        DiffState bdiffStates{ winsize, sourceData, patchData, &destdata };

        fastfile::bdiff::BDiffState state{};
        state.state = &bdiffStates;
        state.type = type;
        while (patchData->CanRead(1)) {
            LOG_TRACE("Pre bdiff");
            if (!bdiff(
                    &state,
                    [](void* state, size_t offset, size_t size) -> uint8_t* {
                        return ((DiffState*)state)->LoadSourceData(offset, size);
                    },
                    [](void* state, size_t offset, size_t size, size_t* pOffset) -> uint8_t* {
                        return ((DiffState*)state)->LoadPatchData(offset, size, pOffset);
                    },
                    [](void* state, size_t size) -> uint8_t* { return ((DiffState*)state)->LoadDestData(size); }
                )) {
                throw std::runtime_error(std::format("bdiff error: {}", state.error));
            }
            if (!bdiffStates.GetLastSize()) {
                break;
            }
        }

        bdiffStates.SyncData();

        LOG_TRACE("bdiff end size: 0x{:x}", destdata.size());
        return destdata;
    }

    namespace {
        // Bounded readers for the parallel path: a patch that runs out of bytes is left to the reference decoder.
        struct PatchCursor {
            const byte* at;
            const byte* end;
            bool ok{ true };

            byte Byte() {
                if (at >= end) {
                    ok = false;
                    return 0;
                }
                return *at++;
            }
            size_t ULEB128() {
                size_t output{};
                for (size_t i = 0; i < 10; i++) {
                    if (at >= end)
                        break;
                    const byte c{ *at++ };
                    output = (output << 7) | (c & 0x7F);
                    if (!(c & 0x80))
                        return output;
                }
                ok = false;
                return 0;
            }
        };

        struct PatchWindow {
            const byte* segment; // source segment (null when the window has none)
            size_t segmentLen;
            const byte* data; // ADD / RUN bytes
            const byte* inst; // instructions
            const byte* addr; // COPY addresses, ends at check
            const byte* check;
            size_t destLen;
            size_t destOffset;
            uint32_t seed; // stored checksum of the previous window
            uint32_t stored;
            bool hasChecksum;
        };

        bool DecodeWindow(const PatchWindow& w, const byte* source, size_t sourceLen, byte* out, BDiffType type) {
            VcdState vcd{};
            PatchCursor inst{ w.inst, w.addr };
            PatchCursor addr{ w.addr, w.check };
            const byte* data{ w.data };
            const byte* const dataEnd{ w.inst };
            byte* const destStart{ out + w.destOffset };
            byte* const destEnd{ destStart + w.destLen };
            byte* dest{ destStart };
            const byte* const sourceEnd{ source + sourceLen };

            while (inst.at < inst.end) {
                const DoubleInstruction* dinst{ &instructions[inst.Byte()] };
                for (size_t i = 0; i < 2; i++) {
                    const Instruction in{ dinst->instruction[i] };
                    size_t ilen{ in.size };
                    if (!ilen && in.op != VCDI_NOOP)
                        ilen = inst.ULEB128();
                    if (!inst.ok)
                        return false;

                    switch (in.op) {
                    case VCDI_NOOP:
                        break;
                    case VCDI_ADD:
                        if (ilen > (size_t)(dataEnd - data) || ilen > (size_t)(destEnd - dest))
                            return false;
                        std::memcpy(dest, data, ilen);
                        data += ilen;
                        dest += ilen;
                        break;
                    case VCDI_RUN:
                        if (data >= dataEnd || ilen > (size_t)(destEnd - dest))
                            return false;
                        std::memset(dest, *(data++), ilen);
                        dest += ilen;
                        break;
                    case VCDI_COPY: {
                        const size_t here{ w.segmentLen + (size_t)(dest - destStart) };
                        size_t address;
                        if (in.mode == VCD_SELF) {
                            address = addr.ULEB128();
                        } else if (in.mode == VCD_HERE) {
                            address = here - addr.ULEB128();
                        } else if (in.mode >= 6) {
                            address = vcd.asame[((size_t)(in.mode - 6) << 8) + addr.Byte()];
                        } else {
                            address = vcd.anear[in.mode - 2] + addr.ULEB128();
                        }
                        if (!addr.ok || ilen > (size_t)(destEnd - dest))
                            return false;
                        vcd.anear[vcd.next_slot] = address;
                        vcd.next_slot = (vcd.next_slot + 1) & 3;
                        vcd.asame[address % 0x300] = address;

                        if (address < w.segmentLen) {
                            // From the source segment (the reference decoder lets a copy run on into the source).
                            if (!w.segment || ilen > (size_t)(sourceEnd - (w.segment + address)))
                                return false;
                            std::memcpy(dest, w.segment + address, ilen);
                        } else {
                            // From this window's own output; an overlapping copy repeats what it just wrote.
                            const size_t back{ address - w.segmentLen };
                            if (back >= (size_t)(dest - destStart))
                                return false;
                            const byte* from{ destStart + back };
                            if (from + ilen <= dest) {
                                std::memcpy(dest, from, ilen);
                            } else {
                                for (size_t k = 0; k < ilen; k++)
                                    dest[k] = from[k];
                            }
                        }
                        dest += ilen;
                        break;
                    }
                    default:
                        return false;
                    }
                }
            }
            if (dest != destEnd)
                return false;

            if (w.hasChecksum) {
                switch (type) {
                case BDT_TREYARCH:
                    if (XXH32(destStart, w.destLen, 0) != w.stored)
                        return false;
                    break;
                case BDT_IW: {
                    const uint64_t xxh64{ XXH64(destStart, w.destLen, w.seed) };
                    if ((uint32_t)(xxh64 ^ (xxh64 >> 32)) != w.stored)
                        return false;
                    break;
                }
                default:
                    break;
                }
            }
            return true;
        }
    } // namespace

    bool bdiffParallel(
        const byte* source, size_t sourceLen, const byte* patch, size_t patchLen, BDiffType type,
        std::unique_ptr<byte[]>& out, size_t& outLen
    ) {
        out.reset();
        outLen = 0;
        if (patchLen < 5 || patch[0] != 0xD6 || patch[1] != 0xC3 || patch[2] != 0xC4 || (patch[4] & 0xF3) != 0)
            return false;
        const byte* const patchEnd{ patch + patchLen };

        // Window headers, in file order.
        std::vector<PatchWindow> windows;
        size_t total{};
        uint32_t lastChecksum{};
        const byte* at{ patch + 5 };
        while (at < patchEnd) {
            PatchCursor header{ at, patchEnd };
            const byte flags{ header.Byte() };
            if ((flags & VCDF_MASK12) == VCDF_MASK12)
                return false;
            PatchWindow w{};
            if (flags & VCDF_MASK12) {
                w.segmentLen = header.ULEB128();
                const size_t locationEx{ header.ULEB128() };
                const size_t location{ flags & VCDF_SOURCE_SEGMENT_RELATIVE ? header.ULEB128() : locationEx };
                if (!header.ok || location > locationEx || location > sourceLen)
                    return false;
                const size_t start{ locationEx - location };
                if (start > sourceLen - location || w.segmentLen > sourceLen - location - start)
                    return false;
                w.segment = source + locationEx;
            }
            const size_t deltaLen{ header.ULEB128() };
            const size_t checkLen{ flags & VCDF_CHECKSUM ? sizeof(uint32_t) : size_t{} };
            if (!header.ok || deltaLen > (size_t)(patchEnd - header.at) ||
                checkLen > (size_t)(patchEnd - header.at) - deltaLen)
                return false;
            PatchCursor delta{ header.at, header.at + deltaLen };
            w.destLen = delta.ULEB128();
            if (delta.Byte() != 0) // compressed sections are not used by the fastfile patches
                return false;
            const size_t dataLen{ delta.ULEB128() };
            const size_t instLen{ delta.ULEB128() };
            const size_t addrLen{ delta.ULEB128() };
            if (!delta.ok)
                return false;
            const size_t sections{ (size_t)(delta.end - delta.at) };
            if (dataLen > sections || instLen > sections - dataLen || addrLen != sections - dataLen - instLen)
                return false;
            w.data = delta.at;
            w.inst = w.data + dataLen;
            w.addr = w.inst + instLen;
            w.check = w.addr + addrLen;
            w.hasChecksum = checkLen != 0;
            w.seed = lastChecksum;
            if (w.hasChecksum) {
                std::memcpy(&w.stored, w.check, sizeof(w.stored));
                lastChecksum = w.stored;
            }
            if (w.destLen > (size_t{ 1 } << 40) || total > (size_t{ 1 } << 40))
                return false;
            w.destOffset = total;
            total += w.destLen;
            at = w.check + checkLen;
            if (!w.destLen)
                break; // the reference decoder stops at an empty window
            windows.push_back(w);
        }

        std::unique_ptr<byte[]> data{ std::make_unique_for_overwrite<byte[]>(std::max<size_t>(total, 1)) };
        std::atomic<size_t> next{ 0 };
        std::atomic<bool> failed{ false };
        auto worker = [&] {
            while (!failed.load(std::memory_order_relaxed)) {
                const size_t i{ next.fetch_add(1, std::memory_order_relaxed) };
                if (i >= windows.size())
                    return;
                if (!DecodeWindow(windows[i], source, sourceLen, data.get(), type))
                    failed.store(true);
            }
        };
        const size_t threads{ std::max<size_t>(
            1,
            std::min({ (size_t)std::thread::hardware_concurrency(), size_t{ 12 }, windows.size(),
                       total / (4 * 1024 * 1024) + 1 })
        ) };
        {
            std::vector<std::thread> pool;
            pool.reserve(threads - 1);
            for (size_t t = 1; t < threads; t++)
                pool.emplace_back(worker);
            worker();
            for (std::thread& t : pool)
                t.join();
        }
        LOG_DEBUG(
            "bdiff: {} window(s), 0x{:x} byte(s), {} thread(s){}", windows.size(), total, threads,
            failed.load() ? ", failed a check" : ""
        );
        if (failed.load())
            return false;

        out = std::move(data);
        outLen = total;
        return true;
    }
} // namespace fastfile::bdiff

bool ActsAPIFastFile_bdiff(
    ActsAPIFastFile_BDiffState* state, ActsAPIFastFile_sourceCallback* sourceDataCB,
    ActsAPIFastFile_diffCallback* patchDataCB, ActsAPIFastFile_destCallback* destDataCB
) {
    return fastfile::bdiff::bdiff(state, sourceDataCB, patchDataCB, destDataCB);
}

ActsStatus ActsAPIFastFile_bdiffData(
    uint8_t* sourceData, size_t sourceDataLen, uint8_t* patchData, size_t patchDataLen, ActsHandle outData,
    ActsAPIFastFile_BDiffType type, size_t winSize
) {
    ACTS_API_ASSERT(sourceData || !sourceDataLen);
    ACTS_API_ASSERT(patchData || !patchDataLen);
    ACTS_API_ASSERT_VALID_HANDLE(outData);

    core::bytebuffer::ByteBuffer sourceDataBuf{ sourceData, sourceDataLen };
    core::bytebuffer::ByteBuffer patchDataBuf{ patchData, patchDataLen };
    try {
        ActsAPIImpl_VectorData(outData) = fastfile::bdiff::bdiff(&sourceDataBuf, &patchDataBuf, type, winSize);
        return ACTS_STATUS_OK;
    } catch (std::runtime_error& e) {
        ActsAPISetLastMessage("%s", e.what());
        return ACTS_STATUS_ERROR;
    }
}
