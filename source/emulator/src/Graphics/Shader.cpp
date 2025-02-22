#include "Emulator/Graphics/Shader.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Profiler.h"

#include "spirv-tools/libspirv.h"
#include "spirv-tools/libspirv.hpp"
#include "spirv-tools/optimizer.hpp"

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

//#define SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
//#include "spirv_cross/spirv_glsl.hpp"

#ifdef KYTY_EMU_ENABLED

#define KYTY_SHADER_PARSER_ARGS                                                                                                            \
	[[maybe_unused]] uint32_t pc, [[maybe_unused]] const uint32_t *src, [[maybe_unused]] const uint32_t *buffer,                           \
	    [[maybe_unused]] ShaderCode *dst
#define KYTY_SHADER_PARSER(f) static uint32_t f(KYTY_SHADER_PARSER_ARGS)
#define KYTY_CP_OP_PARSER_ARGS                                                                                                             \
	[[maybe_unused]] CommandProcessor *cp, [[maybe_unused]] uint32_t cmd_id, [[maybe_unused]] const uint32_t *buffer,                      \
	    [[maybe_unused]] uint32_t dw, [[maybe_unused]] uint32_t num_dw
#define KYTY_CP_OP_PARSER(f) static uint32_t f(KYTY_CP_OP_PARSER_ARGS)

KYTY_ENUM_RANGE(Kyty::Libs::Graphics::ShaderInstructionType, 0, static_cast<int>(Kyty::Libs::Graphics::ShaderInstructionType::ZMax));

namespace Kyty::Libs::Graphics {

struct ShaderBinaryInfo
{
	uint8_t  signature[7];
	uint8_t  version;
	uint32_t pssl_or_cg  : 1;
	uint32_t cached      : 1;
	uint32_t type        : 4;
	uint32_t source_type : 2;
	uint32_t length      : 24;
	uint8_t  chunk_usage_base_offset_dw;
	uint8_t  num_input_usage_slots;
	uint8_t  is_srt                 : 1;
	uint8_t  is_srt_used_info_valid : 1;
	uint8_t  is_extended_usage_info : 1;
	uint8_t  reserved2              : 5;
	uint8_t  reserved3;
	uint32_t hash0;
	uint32_t hash1;
	uint32_t crc32;
};

struct ShaderUsageSlot
{
	uint8_t type;
	uint8_t slot;
	uint8_t start_register;
	uint8_t flags;
};

struct ShaderUsageInfo
{
	const uint32_t*        usage_masks = nullptr;
	const ShaderUsageSlot* slots       = nullptr;
	int                    slots_num   = 0;
};

struct ShaderDebugPrintfCmds
{
	uint64_t                  id = 0;
	Vector<ShaderDebugPrintf> cmds;
};

static Vector<uint64_t>*              g_disabled_shaders = nullptr;
static Vector<ShaderDebugPrintfCmds>* g_debug_printfs    = nullptr;

static String operand_to_str(ShaderOperand op)
{
	String ret = U"???";
	switch (op.type)
	{
		case ShaderOperandType::LiteralConstant:
			EXIT_IF(op.size != 0);
			EXIT_IF(op.negate || op.absolute);
			return String::FromPrintf("%f (%u)", op.constant.f, op.constant.u);
			break;
		case ShaderOperandType::IntegerInlineConstant:
			EXIT_IF(op.size != 0);
			EXIT_IF(op.negate || op.absolute);
			return String::FromPrintf("%d", op.constant.i);
			break;
		case ShaderOperandType::FloatInlineConstant:
			EXIT_IF(op.size != 0);
			EXIT_IF(op.negate || op.absolute);
			return String::FromPrintf("%f", op.constant.f);
			break;
		default: break;
	}

	EXIT_IF(op.size != 1);

	switch (op.type)
	{
		case ShaderOperandType::VccHi: ret = U"vcc_hi"; break;
		case ShaderOperandType::VccLo: ret = U"vcc_lo"; break;
		case ShaderOperandType::ExecHi: ret = U"exec_hi"; break;
		case ShaderOperandType::ExecLo: ret = U"exec_lo"; break;
		case ShaderOperandType::ExecZ: ret = U"execz"; break;
		case ShaderOperandType::Scc: ret = U"scc"; break;
		case ShaderOperandType::M0: ret = U"m0"; break;
		case ShaderOperandType::Vgpr: ret = String::FromPrintf("v%d", op.register_id); break;
		case ShaderOperandType::Sgpr: ret = String::FromPrintf("s%d", op.register_id); break;
		default: break;
	}

	if (op.absolute)
	{
		ret = U"abs(" + ret + U")";
	}

	if (op.negate)
	{
		return U"-" + ret;
	}

	return ret;
}

static String operand_array_to_str(ShaderOperand op, int n)
{
	String ret = U"???";

	EXIT_IF(op.size != n);

	switch (op.type)
	{
		case ShaderOperandType::VccLo:
			if (n == 2)
			{
				ret = U"vcc";
			}
			break;
		case ShaderOperandType::ExecLo:
			if (n == 2)
			{
				ret = U"exec";
			}
			break;
		case ShaderOperandType::Sgpr: ret = String::FromPrintf("s[%d:%d]", op.register_id, op.register_id + n - 1); break;
		case ShaderOperandType::Vgpr: ret = String::FromPrintf("v[%d:%d]", op.register_id, op.register_id + n - 1); break;
		case ShaderOperandType::LiteralConstant:
			if (n == 2)
			{
				ret = String::FromPrintf("%f (%u)", op.constant.f, op.constant.u);
			}
			break;
		case ShaderOperandType::IntegerInlineConstant:
			if (n == 2)
			{
				ret = String::FromPrintf("%d", op.constant.i);
			}
			break;
		default: break;
	}

	EXIT_IF(ret == U"???");

	if (op.absolute)
	{
		ret = U"abs(" + ret + U")";
	}

	if (op.negate)
	{
		return U"-" + ret;
	}

	return ret;
}

static String dbg_fmt_to_str(const ShaderInstruction& inst)
{
	switch (inst.format)
	{
		case ShaderInstructionFormat::Unknown: return U"Unknown"; break;
		case ShaderInstructionFormat::Empty: return U"Empty"; break;
		case ShaderInstructionFormat::Imm: return U"Imm"; break;
		case ShaderInstructionFormat::Mrt0OffOffComprVmDone: return U"Mrt0OffOffComprVmDone"; break;
		case ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone: return U"Mrt0Vsrc0Vsrc1ComprVmDone"; break;
		case ShaderInstructionFormat::Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone: return U"Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone"; break;
		case ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3: return U"Param0Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Param1Vsrc0Vsrc1Vsrc2Vsrc3: return U"Param1Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Param2Vsrc0Vsrc1Vsrc2Vsrc3: return U"Param2Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Param3Vsrc0Vsrc1Vsrc2Vsrc3: return U"Param3Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Param4Vsrc0Vsrc1Vsrc2Vsrc3: return U"Param4Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done: return U"Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done"; break;
		case ShaderInstructionFormat::Saddr: return U"Saddr"; break;
		case ShaderInstructionFormat::Sdst4SbaseSoffset: return U"Sdst4SbaseSoffset"; break;
		case ShaderInstructionFormat::Sdst8SbaseSoffset: return U"Sdst8SbaseSoffset"; break;
		case ShaderInstructionFormat::SdstSvSoffset: return U"SdstSvSoffset"; break;
		case ShaderInstructionFormat::Sdst2SvSoffset: return U"Sdst2SvSoffset"; break;
		case ShaderInstructionFormat::Sdst4SvSoffset: return U"Sdst4SvSoffset"; break;
		case ShaderInstructionFormat::Sdst8SvSoffset: return U"Sdst8SvSoffset"; break;
		case ShaderInstructionFormat::Sdst16SvSoffset: return U"Sdst16SvSoffset"; break;
		case ShaderInstructionFormat::SVdstSVsrc0: return U"SVdstSVsrc0"; break;
		case ShaderInstructionFormat::Sdst2Ssrc02: return U"Sdst2Ssrc02"; break;
		case ShaderInstructionFormat::Sdst2Ssrc02Ssrc12: return U"Sdst2Ssrc02Ssrc12"; break;
		case ShaderInstructionFormat::SmaskVsrc0Vsrc1: return U"SmaskVsrc0Vsrc1"; break;
		case ShaderInstructionFormat::Ssrc0Ssrc1: return U"Ssrc0Ssrc1"; break;
		case ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen: return U"Vdata1VaddrSvSoffsIdxen"; break;
		case ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxenFloat1: return U"Vdata1VaddrSvSoffsIdxenFloat1"; break;
		case ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen: return U"Vdata2VaddrSvSoffsIdxen"; break;
		case ShaderInstructionFormat::Vdata3VaddrSvSoffsIdxen: return U"Vdata3VaddrSvSoffsIdxen"; break;
		case ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen: return U"Vdata4VaddrSvSoffsIdxen"; break;
		case ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxenFloat4: return U"Vdata4VaddrSvSoffsIdxenFloat4"; break;
		case ShaderInstructionFormat::Vdata4Vaddr2SvSoffsOffenIdxenFloat4: return U"Vdata4Vaddr2SvSoffsOffenIdxenFloat4"; break;
		case ShaderInstructionFormat::Vdata1Vaddr3StSsDmask1: return U"Vdata1Vaddr3StSsDmask1"; break;
		case ShaderInstructionFormat::Vdata1Vaddr3StSsDmask8: return U"Vdata1Vaddr3StSsDmask8"; break;
		case ShaderInstructionFormat::Vdata2Vaddr3StSsDmask3: return U"Vdata2Vaddr3StSsDmask3"; break;
		case ShaderInstructionFormat::Vdata2Vaddr3StSsDmask5: return U"Vdata2Vaddr3StSsDmask5"; break;
		case ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7: return U"Vdata3Vaddr3StSsDmask7"; break;
		case ShaderInstructionFormat::Vdata3Vaddr4StSsDmask7: return U"Vdata3Vaddr4StSsDmask7"; break;
		case ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF: return U"Vdata4Vaddr3StSsDmaskF"; break;
		case ShaderInstructionFormat::Vdata4Vaddr3StDmaskF: return U"Vdata4Vaddr3StDmaskF"; break;
		case ShaderInstructionFormat::Vdata4Vaddr4StDmaskF: return U"Vdata4Vaddr4StDmaskF"; break;
		case ShaderInstructionFormat::SVdstSVsrc0SVsrc1: return U"SVdstSVsrc0SVsrc1"; break;
		case ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2: return U"VdstVsrc0Vsrc1Smask2"; break;
		case ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2: return U"VdstVsrc0Vsrc1Vsrc2"; break;
		case ShaderInstructionFormat::VdstVsrcAttrChan: return U"VdstVsrcAttrChan"; break;
		case ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1: return U"VdstSdst2Vsrc0Vsrc1"; break;
		case ShaderInstructionFormat::VdstGds: return U"VdstGds"; break;
		case ShaderInstructionFormat::Label: return U"Label"; break;
		default: return U"????"; break;
	}
}

static String dbg_fmt_print(const ShaderInstruction& inst)
{
	uint64_t f = inst.format;
	EXIT_IF(f == ShaderInstructionFormat::Unknown);
	String str;
	if (f == ShaderInstructionFormat::Empty)
	{
		return str;
	}
	int src_num = 0;
	for (;;)
	{
		String s;
		auto   fu = f & 0xffu;
		if (fu == 0)
		{
			break;
		}
		switch (fu)
		{
			case ShaderInstructionFormat::D: s = operand_to_str(inst.dst); break;
			case ShaderInstructionFormat::D2: s = operand_to_str(inst.dst2); break;
			case ShaderInstructionFormat::S0: s = operand_to_str(inst.src[0]); break;
			case ShaderInstructionFormat::S1: s = operand_to_str(inst.src[1]); break;
			case ShaderInstructionFormat::S2: s = operand_to_str(inst.src[2]); break;
			case ShaderInstructionFormat::S3: s = operand_to_str(inst.src[3]); break;
			case ShaderInstructionFormat::DA2: s = operand_array_to_str(inst.dst, 2); break;
			case ShaderInstructionFormat::DA3: s = operand_array_to_str(inst.dst, 3); break;
			case ShaderInstructionFormat::DA4: s = operand_array_to_str(inst.dst, 4); break;
			case ShaderInstructionFormat::DA8: s = operand_array_to_str(inst.dst, 8); break;
			case ShaderInstructionFormat::DA16: s = operand_array_to_str(inst.dst, 16); break;
			case ShaderInstructionFormat::D2A2: s = operand_array_to_str(inst.dst2, 2); break;
			case ShaderInstructionFormat::D2A3: s = operand_array_to_str(inst.dst2, 3); break;
			case ShaderInstructionFormat::D2A4: s = operand_array_to_str(inst.dst2, 4); break;
			case ShaderInstructionFormat::S0A2: s = operand_array_to_str(inst.src[0], 2); break;
			case ShaderInstructionFormat::S0A3: s = operand_array_to_str(inst.src[0], 3); break;
			case ShaderInstructionFormat::S0A4: s = operand_array_to_str(inst.src[0], 4); break;
			case ShaderInstructionFormat::S1A2: s = operand_array_to_str(inst.src[1], 2); break;
			case ShaderInstructionFormat::S1A3: s = operand_array_to_str(inst.src[1], 3); break;
			case ShaderInstructionFormat::S1A4: s = operand_array_to_str(inst.src[1], 4); break;
			case ShaderInstructionFormat::S1A8: s = operand_array_to_str(inst.src[1], 8); break;
			case ShaderInstructionFormat::S2A2: s = operand_array_to_str(inst.src[2], 2); break;
			case ShaderInstructionFormat::S2A3: s = operand_array_to_str(inst.src[2], 3); break;
			case ShaderInstructionFormat::S2A4: s = operand_array_to_str(inst.src[2], 4); break;
			case ShaderInstructionFormat::Attr: s = String::FromPrintf("attr%u.%u", inst.src[1].constant.u, inst.src[2].constant.u); break;
			case ShaderInstructionFormat::Idxen: s = U"idxen"; break;
			case ShaderInstructionFormat::Offen: s = U"offen"; break;
			case ShaderInstructionFormat::Float1: s = U"format:float1"; break;
			case ShaderInstructionFormat::Float4: s = U"format:float4"; break;
			case ShaderInstructionFormat::Pos0: s = U"pos0"; break;
			case ShaderInstructionFormat::Done: s = U"done"; break;
			case ShaderInstructionFormat::Param0: s = U"param0"; break;
			case ShaderInstructionFormat::Param1: s = U"param1"; break;
			case ShaderInstructionFormat::Param2: s = U"param2"; break;
			case ShaderInstructionFormat::Param3: s = U"param3"; break;
			case ShaderInstructionFormat::Param4: s = U"param4"; break;
			case ShaderInstructionFormat::Mrt0: s = U"mrt_color0"; break;
			case ShaderInstructionFormat::Off: s = U"off"; break;
			case ShaderInstructionFormat::Compr: s = U"compr"; break;
			case ShaderInstructionFormat::Vm: s = U"vm"; break;
			case ShaderInstructionFormat::L: s = String::FromPrintf("label_%04" PRIx32, inst.pc + 4 + inst.src[0].constant.i); break;
			case ShaderInstructionFormat::Dmask1: s = U"dmask:0x1"; break;
			case ShaderInstructionFormat::Dmask8: s = U"dmask:0x8"; break;
			case ShaderInstructionFormat::Dmask3: s = U"dmask:0x3"; break;
			case ShaderInstructionFormat::Dmask5: s = U"dmask:0x5"; break;
			case ShaderInstructionFormat::Dmask7: s = U"dmask:0x7"; break;
			case ShaderInstructionFormat::DmaskF: s = U"dmask:0xf"; break;
			case ShaderInstructionFormat::Gds: s = U"gds"; break;
			default: EXIT("unknown code: %u\n", static_cast<uint32_t>(fu));
		}
		switch (fu)
		{
			case ShaderInstructionFormat::L:
			case ShaderInstructionFormat::S0:
			case ShaderInstructionFormat::S0A2:
			case ShaderInstructionFormat::S0A3:
			case ShaderInstructionFormat::S0A4: src_num = std::max(src_num, 1); break;
			case ShaderInstructionFormat::S1:
			case ShaderInstructionFormat::S1A2:
			case ShaderInstructionFormat::S1A3:
			case ShaderInstructionFormat::S1A4:
			case ShaderInstructionFormat::S1A8: src_num = std::max(src_num, 2); break;
			case ShaderInstructionFormat::S2:
			case ShaderInstructionFormat::S2A2:
			case ShaderInstructionFormat::S2A3:
			case ShaderInstructionFormat::S2A4:
			case ShaderInstructionFormat::Attr: src_num = std::max(src_num, 3); break;
			case ShaderInstructionFormat::S3: src_num = std::max(src_num, 4); break;
			default: break;
		}
		str = s + (str.IsEmpty() ? U"" : U", " + str);
		f >>= 8u;
	}
	EXIT_IF(src_num != inst.src_num);
	if (inst.dst.multiplier == 2.0f)
	{
		str += " mul:2";
	}
	if (inst.dst.multiplier == 4.0f)
	{
		str += " mul:4";
	}
	if (inst.dst.multiplier == 0.5f)
	{
		str += " div:2";
	}
	if (inst.dst.clamp)
	{
		str += U" clamp";
	}
	return str;
}

String ShaderCode::DbgInstructionToStr(const ShaderInstruction& inst)
{
	String ret;

	String name   = Core::EnumName(inst.type);
	String format = dbg_fmt_to_str(inst);

	ret += String::FromPrintf("%-20s [%-30s] ", name.C_Str(), format.C_Str());
	ret += dbg_fmt_print(inst);

	return ret;
}

String ShaderCode::DbgDump() const
{
	String ret;
	for (const auto& inst: m_instructions)
	{
		if (m_labels.Contains(inst.pc, [](auto label, auto pc) { return (!label.IsDisabled() && label.GetDst() == pc); }))
		{
			ret += String::FromPrintf("label_%04" PRIx32 ":\n", inst.pc);
		}
		ret += String::FromPrintf("  %s\n", DbgInstructionToStr(inst).C_Str());
	}
	return ret;
}

bool ShaderCode::IsDiscardInstruction(uint32_t index) const
{
	if (!(index == 0 || index + 1 >= m_instructions.Size()))
	{
		const auto& prev_inst = m_instructions.At(index - 1);
		const auto& inst      = m_instructions.At(index);
		const auto& next_inst = m_instructions.At(index + 1);

		return (inst.type == ShaderInstructionType::Exp && inst.format == ShaderInstructionFormat::Mrt0OffOffComprVmDone &&
		        prev_inst.type == ShaderInstructionType::SMovB64 && prev_inst.format == ShaderInstructionFormat::Sdst2Ssrc02 &&
		        prev_inst.dst.type == ShaderOperandType::ExecLo && prev_inst.src[0].type == ShaderOperandType::IntegerInlineConstant &&
		        prev_inst.src[0].constant.i == 0 && next_inst.type == ShaderInstructionType::SEndpgm);
	}
	return false;
}

bool ShaderCode::IsDiscardBlock(uint32_t pc) const
{
	auto inst_count = m_instructions.Size();
	for (uint32_t index = 0; index < inst_count; index++)
	{
		const auto& inst = m_instructions.At(index);
		if (inst.pc == pc)
		{
			for (uint32_t i = index; i < inst_count; i++)
			{
				const auto& inst = m_instructions.At(i);

				if (inst.type == ShaderInstructionType::SEndpgm || inst.type == ShaderInstructionType::SCbranchExecz ||
				    inst.type == ShaderInstructionType::SCbranchScc0 || inst.type == ShaderInstructionType::SCbranchVccz)
				{
					return false;
				}

				if (IsDiscardInstruction(i))
				{
					return true;
				}
			}
			return false;
		}
	}
	return false;
}

Vector<ShaderInstruction> ShaderCode::GetDiscardBlock(uint32_t pc) const
{
	Vector<ShaderInstruction> ret;

	auto inst_count = m_instructions.Size();
	for (uint32_t index = 0; index < inst_count; index++)
	{
		const auto& inst = m_instructions.At(index);
		if (inst.pc == pc)
		{
			for (uint32_t i = index; i < inst_count; i++)
			{
				const auto& inst = m_instructions.At(i);

				ret.Add(inst);

				if (IsDiscardInstruction(i))
				{
					ret.Add(m_instructions.At(i + 1));
					break;
				}
			}
			break;
		}
	}

	return ret;
}

static ShaderOperand operand_parse(uint32_t code)
{
	ShaderOperand ret;

	ret.size = 1;

	if (code >= 0 && code <= 103)
	{
		ret.type        = ShaderOperandType::Sgpr;
		ret.register_id = static_cast<int>(code);
	} else if (code == 106)
	{
		ret.type = ShaderOperandType::VccLo;
	} else if (code == 107)
	{
		ret.type = ShaderOperandType::VccHi;
	} else if (code == 124)
	{
		ret.type = ShaderOperandType::M0;
	} else if (code == 126)
	{
		ret.type = ShaderOperandType::ExecLo;
	} else if (code == 127)
	{
		ret.type = ShaderOperandType::ExecHi;
	} else if (code >= 128 && code <= 192)
	{
		ret.type       = ShaderOperandType::IntegerInlineConstant;
		ret.constant.i = static_cast<int>(code) - 128;
		ret.size       = 0;
	} else if (code >= 193 && code <= 208)
	{
		ret.type       = ShaderOperandType::IntegerInlineConstant;
		ret.constant.i = 192 - static_cast<int>(code);
		ret.size       = 0;
	} else if (code >= 240 && code <= 247)
	{
		static const float fv[] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f};
		ret.type                = ShaderOperandType::FloatInlineConstant;
		ret.constant.f          = fv[static_cast<int>(code) - 240];
		ret.size                = 0;
	} else if (code == 252)
	{
		ret.type = ShaderOperandType::ExecZ;
	} else if (code == 255)
	{
		ret.type = ShaderOperandType::LiteralConstant;
		ret.size = 0;
	} else if (code >= 256)
	{
		ret.type        = ShaderOperandType::Vgpr;
		ret.register_id = static_cast<int>(code) - 256;
	} else
	{
		EXIT("unknown operand: %u\n", code);
	}

	return ret;
}

KYTY_SHADER_PARSER(shader_parse_sopc)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	uint32_t ssrc1  = (buffer[0] >> 8u) & 0xffu;
	uint32_t ssrc0  = (buffer[0] >> 0u) & 0xffu;
	uint32_t opcode = (buffer[0] >> 16u) & 0x7fu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(ssrc0);
	inst.src[1]  = operand_parse(ssrc1);
	inst.src_num = 2;

	uint32_t size = 1;

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = buffer[size];
		size++;
	}

	if (inst.src[1].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[1].constant.u = buffer[size];
		size++;
	}

	inst.format = ShaderInstructionFormat::Ssrc0Ssrc1;

	switch (opcode)
	{
		case 0x00: inst.type = ShaderInstructionType::SCmpEqI32; break;
		case 0x01: inst.type = ShaderInstructionType::SCmpLgI32; break;
		case 0x02: inst.type = ShaderInstructionType::SCmpGtI32; break;
		case 0x03: inst.type = ShaderInstructionType::SCmpGeI32; break;
		case 0x04: inst.type = ShaderInstructionType::SCmpLtI32; break;
		case 0x05: inst.type = ShaderInstructionType::SCmpLeI32; break;
		case 0x06: inst.type = ShaderInstructionType::SCmpEqU32; break;
		case 0x07: inst.type = ShaderInstructionType::SCmpLgU32; break;
		case 0x08: inst.type = ShaderInstructionType::SCmpGtU32; break;
		case 0x09: inst.type = ShaderInstructionType::SCmpGeU32; break;
		case 0x0a: inst.type = ShaderInstructionType::SCmpLtU32; break;
		case 0x0b: inst.type = ShaderInstructionType::SCmpLeU32; break;

		default: printf("%s", dst->DbgDump().C_Str()); EXIT("unknown sopc opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 "\n", opcode, pc);
	}

	dst->GetInstructions().Add(inst);

	return size;
}

KYTY_SHADER_PARSER(shader_parse_sopk)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	uint32_t opcode = (buffer[0] >> 23u) & 0x1fu;
	auto     imm    = static_cast<int16_t>(buffer[0] >> 0u & 0xffffu);
	uint32_t sdst   = (buffer[0] >> 16u) & 0x7fu;

	ShaderInstruction inst;
	inst.pc  = pc;
	inst.dst = operand_parse(sdst);

	switch (opcode) // NOLINT
	{
		case 0x00:
			inst.type              = ShaderInstructionType::SMovkI32;
			inst.format            = ShaderInstructionFormat::SVdstSVsrc0;
			inst.src[0].type       = ShaderOperandType::IntegerInlineConstant;
			inst.src[0].constant.i = imm;
			inst.src_num           = 1;
			break;
		default: printf("%s", dst->DbgDump().C_Str()); EXIT("unknown sopk opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 "\n", opcode, pc);
	}

	dst->GetInstructions().Add(inst);

	return 1;
}

KYTY_SHADER_PARSER(shader_parse_sopp)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	uint32_t opcode = (buffer[0] >> 16u) & 0x7fu;
	uint32_t simm   = (buffer[0] >> 0u) & 0xffffu;

	ShaderInstruction inst;
	inst.pc = pc;

	inst.format            = ShaderInstructionFormat::Label;
	inst.src[0].type       = ShaderOperandType::LiteralConstant;
	inst.src[0].constant.i = static_cast<int16_t>(simm) * 4;
	inst.src_num           = 1;

	switch (opcode)
	{
		case 0x01:
			inst.type    = ShaderInstructionType::SEndpgm;
			inst.format  = ShaderInstructionFormat::Empty;
			inst.src_num = 0;
			break;
		case 0x04: inst.type = ShaderInstructionType::SCbranchScc0; break;
		case 0x06: inst.type = ShaderInstructionType::SCbranchVccz; break;
		case 0x08: inst.type = ShaderInstructionType::SCbranchExecz; break;
		case 0x0c:
			inst.type              = ShaderInstructionType::SWaitcnt;
			inst.format            = ShaderInstructionFormat::Imm;
			inst.src[0].type       = ShaderOperandType::LiteralConstant;
			inst.src[0].constant.u = simm;
			inst.src_num           = 1;
			break;
		default: printf("%s", dst->DbgDump().C_Str()); EXIT("unknown sopp opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 "\n", opcode, pc);
	}

	dst->GetInstructions().Add(inst);

	if (inst.type == ShaderInstructionType::SCbranchScc0 || inst.type == ShaderInstructionType::SCbranchVccz ||
	    inst.type == ShaderInstructionType::SCbranchExecz)
	{
		dst->GetLabels().Add(ShaderLabel(inst));
	}

	return 1;
}

KYTY_SHADER_PARSER(shader_parse_sop1)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	uint32_t opcode = (buffer[0] >> 8u) & 0xffu;
	uint32_t ssrc0  = (buffer[0] >> 0u) & 0xffu;
	uint32_t sdst   = (buffer[0] >> 16u) & 0x7fu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(ssrc0);
	inst.src_num = 1;
	inst.dst     = operand_parse(sdst);

	uint32_t size = 1;

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = buffer[size];
		size++;
	}

	switch (opcode)
	{
		case 0x03:
			inst.type   = ShaderInstructionType::SMovB32;
			inst.format = ShaderInstructionFormat::SVdstSVsrc0;
			break;
		case 0x04:
			inst.type        = ShaderInstructionType::SMovB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x0a:
			inst.type        = ShaderInstructionType::SWqmB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x20:
			inst.type        = ShaderInstructionType::SSetpcB64;
			inst.format      = ShaderInstructionFormat::Saddr;
			inst.src[0].size = 2;
			break;
		case 0x21:
			inst.type        = ShaderInstructionType::SSwappcB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.src[0].size = 2;
			inst.dst.size    = 2;
			break;
		case 0x24:
			inst.type        = ShaderInstructionType::SAndSaveexecB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		default: printf("%s", dst->DbgDump().C_Str()); EXIT("unknown sop1 opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 "\n", opcode, pc);
	}

	dst->GetInstructions().Add(inst);

	return size;
}

KYTY_SHADER_PARSER(shader_parse_sop2)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	uint32_t opcode = (buffer[0] >> 23u) & 0x7fu;

	switch (opcode)
	{
		case 0x7d: return shader_parse_sop1(pc, src, buffer, dst); break;
		case 0x7e: return shader_parse_sopc(pc, src, buffer, dst); break;
		case 0x7f: return shader_parse_sopp(pc, src, buffer, dst); break;
		default: break;
	}

	if (opcode >= 0x60)
	{
		return shader_parse_sopk(pc, src, buffer, dst);
	}

	uint32_t ssrc1 = (buffer[0] >> 8u) & 0xffu;
	uint32_t ssrc0 = (buffer[0] >> 0u) & 0xffu;
	uint32_t sdst  = (buffer[0] >> 16u) & 0x7fu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(ssrc0);
	inst.src[1]  = operand_parse(ssrc1);
	inst.src_num = 2;
	inst.dst     = operand_parse(sdst);

	uint32_t size = 1;

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = buffer[size];
		size++;
	}

	if (inst.src[1].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[1].constant.u = buffer[size];
		size++;
	}

	inst.format = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;

	switch (opcode)
	{
		case 0x00: inst.type = ShaderInstructionType::SAddU32; break;
		case 0x02: inst.type = ShaderInstructionType::SAddI32; break;
		case 0x04: inst.type = ShaderInstructionType::SAddcU32; break;
		case 0x0a: inst.type = ShaderInstructionType::SCselectB32; break;
		case 0x0b:
			inst.type        = ShaderInstructionType::SCselectB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x0e: inst.type = ShaderInstructionType::SAndB32; break;
		case 0x0f:
			inst.type        = ShaderInstructionType::SAndB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x11:
			inst.type        = ShaderInstructionType::SOrB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x13:
			inst.type        = ShaderInstructionType::SXorB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x15:
			inst.type        = ShaderInstructionType::SAndn2B64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x17:
			inst.type        = ShaderInstructionType::SOrn2B64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x19:
			inst.type        = ShaderInstructionType::SNandB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x1b:
			inst.type        = ShaderInstructionType::SNorB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x1d:
			inst.type        = ShaderInstructionType::SXnorB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x1e: inst.type = ShaderInstructionType::SLshlB32; break;
		case 0x20: inst.type = ShaderInstructionType::SLshrB32; break;
		case 0x24: inst.type = ShaderInstructionType::SBfmB32; break;
		case 0x26: inst.type = ShaderInstructionType::SMulI32; break;
		default: printf("%s", dst->DbgDump().C_Str()); EXIT("unknown sop2 opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 "\n", opcode, pc);
	}

	dst->GetInstructions().Add(inst);

	return size;
}

KYTY_SHADER_PARSER(shader_parse_vopc)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	uint32_t opcode = (buffer[0] >> 17u) & 0xffu;
	uint32_t src0   = (buffer[0] >> 0u) & 0x1ffu;
	uint32_t vsrc1  = (buffer[0] >> 9u) & 0xffu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(src0);
	inst.src[1]  = operand_parse(vsrc1 + 256);
	inst.src_num = 2;

	uint32_t size = 1;

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = buffer[size];
		size++;
	}

	inst.format   = ShaderInstructionFormat::SmaskVsrc0Vsrc1;
	inst.dst.type = ShaderOperandType::VccLo;
	inst.dst.size = 2;

	switch (opcode)
	{
		case 0x00: inst.type = ShaderInstructionType::VCmpFF32; break;
		case 0x01: inst.type = ShaderInstructionType::VCmpLtF32; break;
		case 0x02: inst.type = ShaderInstructionType::VCmpEqF32; break;
		case 0x03: inst.type = ShaderInstructionType::VCmpLeF32; break;
		case 0x04: inst.type = ShaderInstructionType::VCmpGtF32; break;
		case 0x05: inst.type = ShaderInstructionType::VCmpLgF32; break;
		case 0x06: inst.type = ShaderInstructionType::VCmpGeF32; break;
		case 0x07: inst.type = ShaderInstructionType::VCmpOF32; break;
		case 0x08: inst.type = ShaderInstructionType::VCmpUF32; break;
		case 0x09: inst.type = ShaderInstructionType::VCmpNgeF32; break;
		case 0x0a: inst.type = ShaderInstructionType::VCmpNlgF32; break;
		case 0x0b: inst.type = ShaderInstructionType::VCmpNgtF32; break;
		case 0x0c: inst.type = ShaderInstructionType::VCmpNleF32; break;
		case 0x0d: inst.type = ShaderInstructionType::VCmpNeqF32; break;
		case 0x0e: inst.type = ShaderInstructionType::VCmpNltF32; break;
		case 0x0f: inst.type = ShaderInstructionType::VCmpTruF32; break;
		case 0x11: inst.type = ShaderInstructionType::VCmpxLtF32; break;
		case 0x14: inst.type = ShaderInstructionType::VCmpxGtF32; break;
		case 0x1d: inst.type = ShaderInstructionType::VCmpxNeqF32; break;
		case 0x80: inst.type = ShaderInstructionType::VCmpFI32; break;
		case 0x81: inst.type = ShaderInstructionType::VCmpLtI32; break;
		case 0x82: inst.type = ShaderInstructionType::VCmpEqI32; break;
		case 0x83: inst.type = ShaderInstructionType::VCmpLeI32; break;
		case 0x84: inst.type = ShaderInstructionType::VCmpGtI32; break;
		case 0x85: inst.type = ShaderInstructionType::VCmpNeI32; break;
		case 0x86: inst.type = ShaderInstructionType::VCmpGeI32; break;
		case 0x87: inst.type = ShaderInstructionType::VCmpTI32; break;
		case 0xc0: inst.type = ShaderInstructionType::VCmpFU32; break;
		case 0xc1: inst.type = ShaderInstructionType::VCmpLtU32; break;
		case 0xc2: inst.type = ShaderInstructionType::VCmpEqU32; break;
		case 0xc3: inst.type = ShaderInstructionType::VCmpLeU32; break;
		case 0xc4: inst.type = ShaderInstructionType::VCmpGtU32; break;
		case 0xc5: inst.type = ShaderInstructionType::VCmpNeU32; break;
		case 0xc6: inst.type = ShaderInstructionType::VCmpGeU32; break;
		case 0xc7: inst.type = ShaderInstructionType::VCmpTU32; break;
		case 0xd2: inst.type = ShaderInstructionType::VCmpxEqU32; break;
		case 0xd4: inst.type = ShaderInstructionType::VCmpxGtU32; break;
		case 0xd5: inst.type = ShaderInstructionType::VCmpxNeU32; break;
		case 0xd6: inst.type = ShaderInstructionType::VCmpxGeU32; break;
		default:
			printf("%s", dst->DbgDump().C_Str());
			EXIT("unknown vopc opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 " (hash0 = 0x%08" PRIx32 ")\n", opcode, pc, dst->GetHash0());
	}

	dst->GetInstructions().Add(inst);

	return size;
}

KYTY_SHADER_PARSER(shader_parse_vop1)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	uint32_t vdst   = (buffer[0] >> 17u) & 0xffu;
	uint32_t src0   = (buffer[0] >> 0u) & 0x1ffu;
	uint32_t opcode = (buffer[0] >> 9u) & 0xffu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(src0);
	inst.dst     = operand_parse(vdst + 256);
	inst.src_num = 1;

	uint32_t size = 1;

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = buffer[size];
		size++;
	}

	inst.format = ShaderInstructionFormat::SVdstSVsrc0;

	switch (opcode)
	{
		case 0x01: inst.type = ShaderInstructionType::VMovB32; break;
		case 0x05: inst.type = ShaderInstructionType::VCvtF32I32; break;
		case 0x06: inst.type = ShaderInstructionType::VCvtF32U32; break;
		case 0x07: inst.type = ShaderInstructionType::VCvtU32F32; break;
		case 0x0b: inst.type = ShaderInstructionType::VCvtF32F16; break;
		case 0x11: inst.type = ShaderInstructionType::VCvtF32Ubyte0; break;
		case 0x12: inst.type = ShaderInstructionType::VCvtF32Ubyte1; break;
		case 0x13: inst.type = ShaderInstructionType::VCvtF32Ubyte2; break;
		case 0x14: inst.type = ShaderInstructionType::VCvtF32Ubyte3; break;
		case 0x20: inst.type = ShaderInstructionType::VFractF32; break;
		case 0x21: inst.type = ShaderInstructionType::VTruncF32; break;
		case 0x22: inst.type = ShaderInstructionType::VCeilF32; break;
		case 0x23: inst.type = ShaderInstructionType::VRndneF32; break;
		case 0x24: inst.type = ShaderInstructionType::VFloorF32; break;
		case 0x25: inst.type = ShaderInstructionType::VExpF32; break;
		case 0x27: inst.type = ShaderInstructionType::VLogF32; break;
		case 0x2a: inst.type = ShaderInstructionType::VRcpF32; break;
		case 0x2e: inst.type = ShaderInstructionType::VRsqF32; break;
		case 0x33: inst.type = ShaderInstructionType::VSqrtF32; break;
		case 0x35: inst.type = ShaderInstructionType::VSinF32; break;
		case 0x36: inst.type = ShaderInstructionType::VCosF32; break;
		case 0x37: inst.type = ShaderInstructionType::VNotB32; break;
		case 0x38: inst.type = ShaderInstructionType::VBfrevB32; break;
		default:
			printf("%s", dst->DbgDump().C_Str());
			EXIT("unknown vop1 opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 " (hash0 = 0x%08" PRIx32 ")\n", opcode, pc, dst->GetHash0());
	}

	dst->GetInstructions().Add(inst);

	return size;
}

KYTY_SHADER_PARSER(shader_parse_vop2)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	uint32_t opcode = (buffer[0] >> 25u) & 0x3fu;
	uint32_t vdst   = (buffer[0] >> 17u) & 0xffu;
	uint32_t src0   = (buffer[0] >> 0u) & 0x1ffu;
	uint32_t vsrc1  = (buffer[0] >> 9u) & 0xffu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(src0);
	inst.src[1]  = operand_parse(vsrc1 + 256);
	inst.dst     = operand_parse(vdst + 256);
	inst.src_num = 2;

	uint32_t size = 1;

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = buffer[size];
		size++;
	}

	inst.format = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;

	switch (opcode)
	{
		case 0x00:
			inst.type        = ShaderInstructionType::VCndmaskB32;
			inst.format      = ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2;
			inst.src[2].type = ShaderOperandType::VccLo;
			inst.src[2].size = 2;
			inst.src_num     = 3;
			break;
		case 0x03: inst.type = ShaderInstructionType::VAddF32; break;
		case 0x04: inst.type = ShaderInstructionType::VSubF32; break;
		case 0x05: inst.type = ShaderInstructionType::VSubrevF32; break;
		case 0x08: inst.type = ShaderInstructionType::VMulF32; break;
		case 0x0b: inst.type = ShaderInstructionType::VMulU32U24; break;
		case 0x0f: inst.type = ShaderInstructionType::VMinF32; break;
		case 0x10: inst.type = ShaderInstructionType::VMaxF32; break;
		case 0x15: inst.type = ShaderInstructionType::VLshrB32; break;
		case 0x16: inst.type = ShaderInstructionType::VLshrrevB32; break;
		case 0x17: inst.type = ShaderInstructionType::VAshrI32; break;
		case 0x18: inst.type = ShaderInstructionType::VAshrrevI32; break;
		case 0x19: inst.type = ShaderInstructionType::VLshlB32; break;
		case 0x1a: inst.type = ShaderInstructionType::VLshlrevB32; break;
		case 0x1b: inst.type = ShaderInstructionType::VAndB32; break;
		case 0x1c: inst.type = ShaderInstructionType::VOrB32; break;
		case 0x1d: inst.type = ShaderInstructionType::VXorB32; break;
		case 0x1e: inst.type = ShaderInstructionType::VBfmB32; break;
		case 0x1f: inst.type = ShaderInstructionType::VMacF32; break;
		case 0x20:
			inst.type              = ShaderInstructionType::VMadmkF32;
			inst.format            = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;
			inst.src_num           = 3;
			inst.src[2]            = inst.src[1];
			inst.src[1].type       = ShaderOperandType::LiteralConstant;
			inst.src[1].constant.u = buffer[size];
			inst.src[1].size       = 0;
			size++;
			break;
		case 0x21:
			inst.type              = ShaderInstructionType::VMadakF32;
			inst.format            = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;
			inst.src_num           = 3;
			inst.src[2].type       = ShaderOperandType::LiteralConstant;
			inst.src[2].constant.u = buffer[size];
			inst.src[2].size       = 0;
			size++;
			break;
		case 0x22: inst.type = ShaderInstructionType::VBcntU32B32; break;
		case 0x23: inst.type = ShaderInstructionType::VMbcntLoU32B32; break;
		case 0x24: inst.type = ShaderInstructionType::VMbcntHiU32B32; break;
		case 0x25:
			inst.type      = ShaderInstructionType::VAddI32;
			inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
			inst.dst2.type = ShaderOperandType::VccLo;
			inst.dst2.size = 2;
			break;
		case 0x26:
			inst.type      = ShaderInstructionType::VSubI32;
			inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
			inst.dst2.type = ShaderOperandType::VccLo;
			inst.dst2.size = 2;
			break;
		case 0x27:
			inst.type      = ShaderInstructionType::VSubrevI32;
			inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
			inst.dst2.type = ShaderOperandType::VccLo;
			inst.dst2.size = 2;
			break;
		case 0x2f: inst.type = ShaderInstructionType::VCvtPkrtzF16F32; break;
		case 0x3e: return shader_parse_vopc(pc, src, buffer, dst); break;
		case 0x3f: return shader_parse_vop1(pc, src, buffer, dst); break;
		default: printf("%s", dst->DbgDump().C_Str()); EXIT("unknown vop2 opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 "\n", opcode, pc);
	}

	dst->GetInstructions().Add(inst);

	return size;
}

KYTY_SHADER_PARSER(shader_parse_vop3)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	uint32_t opcode = (buffer[0] >> 17u) & 0x1ffu;
	uint32_t clamp  = (buffer[0] >> 11u) & 0x1u;
	uint32_t abs    = (buffer[0] >> 8u) & 0x7u;
	uint32_t vdst   = (buffer[0] >> 0u) & 0xffu;
	uint32_t sdst   = (buffer[0] >> 8u) & 0x7fu;
	uint32_t neg    = (buffer[1] >> 29u) & 0x7u;
	uint32_t omod   = (buffer[1] >> 27u) & 0x3u;
	uint32_t src0   = (buffer[1] >> 0u) & 0x1ffu;
	uint32_t src1   = (buffer[1] >> 9u) & 0x1ffu;
	uint32_t src2   = (buffer[1] >> 18u) & 0x1ffu;

	// EXIT_NOT_IMPLEMENTED(abs != 0);
	// EXIT_NOT_IMPLEMENTED(sdst != 0);

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(src0);
	inst.src[1]  = operand_parse(src1);
	inst.src[2]  = operand_parse(src2);
	inst.src_num = 3;
	inst.dst     = operand_parse(vdst + 256);

	switch (omod)
	{
		case 0: inst.dst.multiplier = 1.0f; break;
		case 1: inst.dst.multiplier = 2.0f; break;
		case 2: inst.dst.multiplier = 4.0f; break;
		case 3: inst.dst.multiplier = 0.5f; break;
		default: break;
	}

	if ((neg & 0x1u) != 0)
	{
		inst.src[0].negate = true;
	}
	if ((neg & 0x2u) != 0)
	{
		inst.src[1].negate = true;
	}
	if ((neg & 0x4u) != 0)
	{
		inst.src[2].negate = true;
	}

	uint32_t size = 2;

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = buffer[size];
		size++;
	}

	if (inst.src[1].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[1].constant.u = buffer[size];
		size++;
	}

	if (inst.src[2].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[2].constant.u = buffer[size];
		size++;
	}

	inst.format = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;

	if (opcode >= 0 && opcode <= 0xff)
	{
		inst.format   = ShaderInstructionFormat::SmaskVsrc0Vsrc1;
		inst.src_num  = 2;
		inst.dst      = operand_parse(vdst);
		inst.dst.size = 2;
	}

	if (opcode >= 0x100 && opcode <= 0x13d)
	{
		inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
		inst.src_num = 2;
	}

	switch (opcode)
	{
		case 0x00: inst.type = ShaderInstructionType::VCmpFF32; break;
		case 0x01: inst.type = ShaderInstructionType::VCmpLtF32; break;
		case 0x02: inst.type = ShaderInstructionType::VCmpEqF32; break;
		case 0x03: inst.type = ShaderInstructionType::VCmpLeF32; break;
		case 0x04: inst.type = ShaderInstructionType::VCmpGtF32; break;
		case 0x05: inst.type = ShaderInstructionType::VCmpLgF32; break;
		case 0x06: inst.type = ShaderInstructionType::VCmpGeF32; break;
		case 0x07: inst.type = ShaderInstructionType::VCmpOF32; break;
		case 0x08: inst.type = ShaderInstructionType::VCmpUF32; break;
		case 0x09: inst.type = ShaderInstructionType::VCmpNgeF32; break;
		case 0x0a: inst.type = ShaderInstructionType::VCmpNlgF32; break;
		case 0x0b: inst.type = ShaderInstructionType::VCmpNgtF32; break;
		case 0x0c: inst.type = ShaderInstructionType::VCmpNleF32; break;
		case 0x0d: inst.type = ShaderInstructionType::VCmpNeqF32; break;
		case 0x0e: inst.type = ShaderInstructionType::VCmpNltF32; break;
		case 0x0f: inst.type = ShaderInstructionType::VCmpTruF32; break;
		case 0x1d: inst.type = ShaderInstructionType::VCmpxNeqF32; break;
		case 0x80: inst.type = ShaderInstructionType::VCmpFI32; break;
		case 0x81: inst.type = ShaderInstructionType::VCmpLtI32; break;
		case 0x82: inst.type = ShaderInstructionType::VCmpEqI32; break;
		case 0x83: inst.type = ShaderInstructionType::VCmpLeI32; break;
		case 0x84: inst.type = ShaderInstructionType::VCmpGtI32; break;
		case 0x85: inst.type = ShaderInstructionType::VCmpNeI32; break;
		case 0x86: inst.type = ShaderInstructionType::VCmpGeI32; break;
		case 0x87: inst.type = ShaderInstructionType::VCmpTI32; break;
		case 0xc0: inst.type = ShaderInstructionType::VCmpFU32; break;
		case 0xc1: inst.type = ShaderInstructionType::VCmpLtU32; break;
		case 0xc2: inst.type = ShaderInstructionType::VCmpEqU32; break;
		case 0xc3: inst.type = ShaderInstructionType::VCmpLeU32; break;
		case 0xc4: inst.type = ShaderInstructionType::VCmpGtU32; break;
		case 0xc5: inst.type = ShaderInstructionType::VCmpNeU32; break;
		case 0xc6: inst.type = ShaderInstructionType::VCmpGeU32; break;
		case 0xc7: inst.type = ShaderInstructionType::VCmpTU32; break;
		case 0xd2: inst.type = ShaderInstructionType::VCmpxEqU32; break;
		case 0xd4: inst.type = ShaderInstructionType::VCmpxGtU32; break;
		case 0xd5: inst.type = ShaderInstructionType::VCmpxNeU32; break;
		case 0xd6: inst.type = ShaderInstructionType::VCmpxGeU32; break;
		case 0x100:
			inst.type        = ShaderInstructionType::VCndmaskB32;
			inst.format      = ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2;
			inst.src_num     = 3;
			inst.src[2].size = 2;
			break;
		case 0x103: inst.type = ShaderInstructionType::VAddF32; break;
		case 0x104: inst.type = ShaderInstructionType::VSubF32; break;
		case 0x105: inst.type = ShaderInstructionType::VSubrevF32; break;
		case 0x108: inst.type = ShaderInstructionType::VMulF32; break;
		case 0x10b: inst.type = ShaderInstructionType::VMulU32U24; break;
		case 0x10f: inst.type = ShaderInstructionType::VMinF32; break;
		case 0x110: inst.type = ShaderInstructionType::VMaxF32; break;
		case 0x115: inst.type = ShaderInstructionType::VLshrB32; break;
		case 0x116: inst.type = ShaderInstructionType::VLshrrevB32; break;
		case 0x117: inst.type = ShaderInstructionType::VAshrI32; break;
		case 0x118: inst.type = ShaderInstructionType::VAshrrevI32; break;
		case 0x119: inst.type = ShaderInstructionType::VLshlB32; break;
		case 0x11a: inst.type = ShaderInstructionType::VLshlrevB32; break;
		case 0x11b: inst.type = ShaderInstructionType::VAndB32; break;
		case 0x11c: inst.type = ShaderInstructionType::VOrB32; break;
		case 0x11d: inst.type = ShaderInstructionType::VXorB32; break;
		case 0x11e: inst.type = ShaderInstructionType::VBfmB32; break;
		case 0x11f: inst.type = ShaderInstructionType::VMacF32; break;
		case 0x122: inst.type = ShaderInstructionType::VBcntU32B32; break;
		case 0x123: inst.type = ShaderInstructionType::VMbcntLoU32B32; break;
		case 0x124: inst.type = ShaderInstructionType::VMbcntHiU32B32; break;
		case 0x125:
			inst.type      = ShaderInstructionType::VAddI32;
			inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
			inst.dst2      = operand_parse(sdst);
			inst.dst2.size = 2;
			break;
		case 0x126:
			inst.type      = ShaderInstructionType::VSubI32;
			inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
			inst.dst2      = operand_parse(sdst);
			inst.dst2.size = 2;
			break;
		case 0x127:
			inst.type      = ShaderInstructionType::VSubrevI32;
			inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
			inst.dst2      = operand_parse(sdst);
			inst.dst2.size = 2;
			break;
		case 0x12f: inst.type = ShaderInstructionType::VCvtPkrtzF16F32; break;
		case 0x141: inst.type = ShaderInstructionType::VMadF32; break;
		case 0x143: inst.type = ShaderInstructionType::VMadU32U24; break;
		case 0x148: inst.type = ShaderInstructionType::VBfeU32; break;
		case 0x14b: inst.type = ShaderInstructionType::VFmaF32; break;
		case 0x151: inst.type = ShaderInstructionType::VMin3F32; break;
		case 0x154: inst.type = ShaderInstructionType::VMax3F32; break;
		case 0x157: inst.type = ShaderInstructionType::VMed3F32; break;
		case 0x15d: inst.type = ShaderInstructionType::VSadU32; break;
		case 0x169:
			inst.type    = ShaderInstructionType::VMulLoU32;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
			inst.src_num = 2;
			break;
		case 0x16a:
			inst.type    = ShaderInstructionType::VMulHiU32;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
			inst.src_num = 2;
			break;
		case 0x16b:
			inst.type    = ShaderInstructionType::VMulLoI32;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
			inst.src_num = 2;
			break;
		case 0x1aa:
			inst.type    = ShaderInstructionType::VRcpF32;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0;
			inst.src_num = 1;
			break;
		default:
			printf("%s", dst->DbgDump().C_Str());
			EXIT("unknown vop3 opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 " (hash0 = 0x%08" PRIx32 ")\n", opcode, pc, dst->GetHash0());
	}

	if (inst.dst2.type == ShaderOperandType::Unknown)
	{
		if ((abs & 0x1u) != 0)
		{
			inst.src[0].absolute = true;
		}
		if ((abs & 0x2u) != 0)
		{
			inst.src[1].absolute = true;
		}
		if ((abs & 0x4u) != 0)
		{
			inst.src[2].absolute = true;
		}

		inst.dst.clamp = (clamp != 0);
	}

	dst->GetInstructions().Add(inst);

	return size;
}

KYTY_SHADER_PARSER(shader_parse_exp)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	uint32_t vm     = (buffer[0] >> 12u) & 0x1u;
	uint32_t done   = (buffer[0] >> 11u) & 0x1u;
	uint32_t compr  = (buffer[0] >> 10u) & 0x1u;
	uint32_t target = (buffer[0] >> 4u) & 0x3fu;
	uint32_t en     = (buffer[0] >> 0u) & 0xfu;

	uint32_t vsrc0 = (buffer[1] >> 0u) & 0xffu;
	uint32_t vsrc1 = (buffer[1] >> 8u) & 0xffu;
	uint32_t vsrc2 = (buffer[1] >> 16u) & 0xffu;
	uint32_t vsrc3 = (buffer[1] >> 24u) & 0xffu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(vsrc0 + 256);
	inst.src[1]  = operand_parse(vsrc1 + 256);
	inst.src[2]  = operand_parse(vsrc2 + 256);
	inst.src[3]  = operand_parse(vsrc3 + 256);
	inst.src_num = 4;

	inst.type = ShaderInstructionType::Exp;

	switch (target)
	{
		case 0x00:
			if (done != 0 && compr != 0 && vm != 0 && en == 0x0)
			{
				inst.format  = ShaderInstructionFormat::Mrt0OffOffComprVmDone;
				inst.src_num = 0;
			} else if (done != 0 && compr != 0 && vm != 0 && en == 0xf)
			{
				inst.format  = ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone;
				inst.src_num = 2;
			} else if (done != 0 && compr == 0 && vm != 0 && en == 0xf)
			{
				inst.format = ShaderInstructionFormat::Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone;
			};
			break;
		case 0x0c:
			if (done != 0 && en == 0xf)
			{
				inst.format = ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done;
			};
			break;
		default: break;
	}

	if (inst.format == ShaderInstructionFormat::Unknown && done == 0 && compr == 0 && vm == 0 && en == 0xf)
	{
		switch (target)
		{
			case 0x20: inst.format = ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x21: inst.format = ShaderInstructionFormat::Param1Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x22: inst.format = ShaderInstructionFormat::Param2Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x23: inst.format = ShaderInstructionFormat::Param3Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x24: inst.format = ShaderInstructionFormat::Param4Vsrc0Vsrc1Vsrc2Vsrc3; break;
			default: break;
		}
	}

	if (inst.format == ShaderInstructionFormat::Unknown)
	{
		printf("%s", dst->DbgDump().C_Str());
		EXIT("%s\n"
		     "unknown exp target: 0x%02" PRIx32 " at addr 0x%08" PRIx32 " (hash0 = 0x%08" PRIx32 ")\n",
		     dst->DbgDump().C_Str(), target, pc, dst->GetHash0());
	}

	dst->GetInstructions().Add(inst);

	return 2;
}

KYTY_SHADER_PARSER(shader_parse_smrd)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	uint32_t opcode = (buffer[0] >> 22u) & 0x1fu;
	uint32_t sdst   = (buffer[0] >> 15u) & 0x7fu;
	uint32_t sbase  = (buffer[0] >> 9u) & 0x3fu;
	uint32_t imm    = (buffer[0] >> 8u) & 0x1u;
	uint32_t offset = (buffer[0] >> 0u) & 0xffu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.dst     = operand_parse(sdst);
	inst.src_num = 2;
	inst.src[0]  = operand_parse(sbase * 2);

	uint32_t size = 1;

	if (imm == 1)
	{
		inst.src[1].type       = ShaderOperandType::LiteralConstant;
		inst.src[1].constant.u = offset << 2u;
	} else
	{
		inst.src[1] = operand_parse(offset);

		if (inst.src[1].type == ShaderOperandType::LiteralConstant)
		{
			inst.src[1].constant.u = buffer[size];
			size++;
		}
	}

	switch (opcode)
	{
		case 0x02:
			inst.type        = ShaderInstructionType::SLoadDwordx4;
			inst.format      = ShaderInstructionFormat::Sdst4SbaseSoffset;
			inst.src[0].size = 2;
			inst.dst.size    = 4;
			break;
		case 0x03:
			inst.type        = ShaderInstructionType::SLoadDwordx8;
			inst.format      = ShaderInstructionFormat::Sdst8SbaseSoffset;
			inst.src[0].size = 2;
			inst.dst.size    = 8;
			break;
		case 0x08:
			inst.type        = ShaderInstructionType::SBufferLoadDword;
			inst.format      = ShaderInstructionFormat::SdstSvSoffset;
			inst.src[0].size = 4;
			break;
		case 0x09:
			inst.type        = ShaderInstructionType::SBufferLoadDwordx2;
			inst.format      = ShaderInstructionFormat::Sdst2SvSoffset;
			inst.src[0].size = 4;
			inst.dst.size    = 2;
			break;
		case 0x0a:
			inst.type        = ShaderInstructionType::SBufferLoadDwordx4;
			inst.format      = ShaderInstructionFormat::Sdst4SvSoffset;
			inst.src[0].size = 4;
			inst.dst.size    = 4;
			break;
		case 0x0b:
			inst.type        = ShaderInstructionType::SBufferLoadDwordx8;
			inst.format      = ShaderInstructionFormat::Sdst8SvSoffset;
			inst.src[0].size = 4;
			inst.dst.size    = 8;
			break;
		case 0x0c:
			inst.type        = ShaderInstructionType::SBufferLoadDwordx16;
			inst.format      = ShaderInstructionFormat::Sdst16SvSoffset;
			inst.src[0].size = 4;
			inst.dst.size    = 16;
			break;
		default: printf("%s", dst->DbgDump().C_Str()); EXIT("unknown smrd opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 "\n", opcode, pc);
	}

	dst->GetInstructions().Add(inst);

	return size;
}

KYTY_SHADER_PARSER(shader_parse_mubuf)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	uint32_t opcode = (buffer[0] >> 18u) & 0x1fu;
	uint32_t lds    = (buffer[0] >> 16u) & 0x1u;
	uint32_t glc    = (buffer[0] >> 14u) & 0x1u;
	uint32_t idxen  = (buffer[0] >> 13u) & 0x1u;
	uint32_t offen  = (buffer[0] >> 12u) & 0x1u;
	uint32_t offset = (buffer[0] >> 0u) & 0xfffu;

	uint32_t soffset = (buffer[1] >> 24u) & 0xffu;
	uint32_t tfe     = (buffer[1] >> 23u) & 0x1u;
	uint32_t slc     = (buffer[1] >> 22u) & 0x1u;
	uint32_t srsrc   = (buffer[1] >> 16u) & 0x1fu;
	uint32_t vdata   = (buffer[1] >> 8u) & 0xffu;
	uint32_t vaddr   = (buffer[1] >> 0u) & 0xffu;

	EXIT_NOT_IMPLEMENTED(idxen == 0);
	EXIT_NOT_IMPLEMENTED(offen == 1);
	EXIT_NOT_IMPLEMENTED(offset != 0);
	EXIT_NOT_IMPLEMENTED(glc == 1);
	EXIT_NOT_IMPLEMENTED(slc == 1);
	EXIT_NOT_IMPLEMENTED(lds == 1);
	EXIT_NOT_IMPLEMENTED(tfe == 1);

	uint32_t size = 2;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.dst     = operand_parse(vdata + 256);
	inst.src_num = 3;
	inst.src[0]  = operand_parse(vaddr + 256);
	inst.src[1]  = operand_parse(srsrc * 4);
	inst.src[2]  = operand_parse(soffset);

	if (inst.src[2].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[2].constant.u = buffer[size];
		size++;
	}

	switch (opcode)
	{
		case 0x00:
			inst.type        = ShaderInstructionType::BufferLoadFormatX;
			inst.format      = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			break;
		case 0x01:
			inst.type        = ShaderInstructionType::BufferLoadFormatXy;
			inst.format      = ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 2;
			break;
		case 0x02:
			inst.type        = ShaderInstructionType::BufferLoadFormatXyz;
			inst.format      = ShaderInstructionFormat::Vdata3VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 3;
			break;
		case 0x03:
			inst.type        = ShaderInstructionType::BufferLoadFormatXyzw;
			inst.format      = ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 4;
			break;
		case 0x04:
			inst.type        = ShaderInstructionType::BufferStoreFormatX;
			inst.format      = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			break;
		case 0x05:
			inst.type        = ShaderInstructionType::BufferStoreFormatXy;
			inst.format      = ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 2;
			break;
		case 0x0c:
			inst.type        = ShaderInstructionType::BufferLoadDword;
			inst.format      = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			break;
		case 0x1c:
			inst.type        = ShaderInstructionType::BufferStoreDword;
			inst.format      = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			break;
		default: printf("%s", dst->DbgDump().C_Str()); EXIT("unknown mubuf opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 "\n", opcode, pc);
	}

	dst->GetInstructions().Add(inst);

	return size;
}

KYTY_SHADER_PARSER(shader_parse_ds)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	uint32_t opcode  = (buffer[0] >> 18u) & 0xffu;
	uint32_t gds     = (buffer[0] >> 17u) & 0x1u;
	uint32_t offset0 = (buffer[0] >> 0u) & 0xffu;
	uint32_t offset1 = (buffer[0] >> 8u) & 0xffu;

	uint32_t vdst  = (buffer[1] >> 24u) & 0xffu;
	uint32_t data1 = (buffer[1] >> 16u) & 0xffu;
	uint32_t data0 = (buffer[1] >> 8u) & 0xffu;
	uint32_t addr  = (buffer[1] >> 0u) & 0xffu;

	EXIT_NOT_IMPLEMENTED(addr != 0);
	EXIT_NOT_IMPLEMENTED(data0 != 0);
	EXIT_NOT_IMPLEMENTED(data1 != 0);
	EXIT_NOT_IMPLEMENTED(offset0 != 0);
	EXIT_NOT_IMPLEMENTED(offset1 != 0);
	EXIT_NOT_IMPLEMENTED(gds == 0);

	uint32_t size = 2;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.dst     = operand_parse(vdst + 256);
	inst.src_num = 0;

	switch (opcode) // NOLINT
	{
		case 0x3d:
			inst.type   = ShaderInstructionType::DsConsume;
			inst.format = ShaderInstructionFormat::VdstGds;
			break;
		case 0x3e:
			inst.type   = ShaderInstructionType::DsAppend;
			inst.format = ShaderInstructionFormat::VdstGds;
			break;
		default: printf("%s", dst->DbgDump().C_Str()); EXIT("unknown ds opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 "\n", opcode, pc);
	}

	dst->GetInstructions().Add(inst);

	return size;
}

KYTY_SHADER_PARSER(shader_parse_mimg)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	uint32_t slc    = (buffer[0] >> 25u) & 0x1u;
	uint32_t opcode = (buffer[0] >> 18u) & 0x7fu;
	uint32_t lwe    = (buffer[0] >> 17u) & 0x1u;
	uint32_t tff    = (buffer[0] >> 16u) & 0x1u;
	uint32_t r128   = (buffer[0] >> 15u) & 0x1u;
	uint32_t da     = (buffer[0] >> 14u) & 0x1u;
	uint32_t glc    = (buffer[0] >> 13u) & 0x1u;
	uint32_t unrm   = (buffer[0] >> 12u) & 0x1u;
	uint32_t dmask  = (buffer[0] >> 8u) & 0xfu;

	uint32_t ssamp = (buffer[1] >> 21u) & 0x1fu; // S#
	uint32_t srsrc = (buffer[1] >> 16u) & 0x1fu; // T#
	uint32_t vdata = (buffer[1] >> 8u) & 0xffu;
	uint32_t vaddr = (buffer[1] >> 0u) & 0xffu;

	EXIT_NOT_IMPLEMENTED(da == 1);
	EXIT_NOT_IMPLEMENTED(r128 == 1);
	EXIT_NOT_IMPLEMENTED(tff == 1);
	EXIT_NOT_IMPLEMENTED(lwe == 1);
	EXIT_NOT_IMPLEMENTED(glc == 1);
	EXIT_NOT_IMPLEMENTED(slc == 1);
	EXIT_NOT_IMPLEMENTED(unrm == 1);
	// EXIT_NOT_IMPLEMENTED(dmask != 0xf && dmask != 0x7);

	uint32_t size = 2;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.dst     = operand_parse(vdata + 256);
	inst.src_num = 3;
	inst.src[0]  = operand_parse(vaddr + 256);
	inst.src[1]  = operand_parse(srsrc * 4);
	inst.src[2]  = operand_parse(ssamp * 4);

	switch (opcode)
	{
		case 0x00:
			inst.type        = ShaderInstructionType::ImageLoad;
			inst.src[0].size = 3;
			inst.src[1].size = 8;
			inst.src_num     = 2;
			if (dmask == 0xf)
			{
				inst.format   = ShaderInstructionFormat::Vdata4Vaddr3StDmaskF;
				inst.dst.size = 4;
			}
			break;
		case 0x08:
			inst.type        = ShaderInstructionType::ImageStore;
			inst.src[0].size = 3;
			inst.src[1].size = 8;
			inst.src_num     = 2;
			if (dmask == 0xf)
			{
				inst.format   = ShaderInstructionFormat::Vdata4Vaddr3StDmaskF;
				inst.dst.size = 4;
			}
			break;
		case 0x09:
			inst.type        = ShaderInstructionType::ImageStoreMip;
			inst.src[0].size = 4;
			inst.src[1].size = 8;
			inst.src_num     = 2;
			if (dmask == 0xf)
			{
				inst.format   = ShaderInstructionFormat::Vdata4Vaddr4StDmaskF;
				inst.dst.size = 4;
			}
			break;
		case 0x20:
			inst.type        = ShaderInstructionType::ImageSample;
			inst.src[0].size = 3;
			inst.src[1].size = 8;
			inst.src[2].size = 4;
			switch (dmask)
			{
				case 0x1:
				{
					inst.format   = ShaderInstructionFormat::Vdata1Vaddr3StSsDmask1;
					inst.dst.size = 1;
					break;
				}
				case 0x3:
				{
					inst.format   = ShaderInstructionFormat::Vdata2Vaddr3StSsDmask3;
					inst.dst.size = 2;
					break;
				}
				case 0x5:
				{
					inst.format   = ShaderInstructionFormat::Vdata2Vaddr3StSsDmask5;
					inst.dst.size = 2;
					break;
				}
				case 0x7:
				{
					inst.format   = ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7;
					inst.dst.size = 3;
					break;
				}
				case 0x8:
				{
					inst.format   = ShaderInstructionFormat::Vdata1Vaddr3StSsDmask8;
					inst.dst.size = 1;
					break;
				}
				case 0xf:
				{
					inst.format   = ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF;
					inst.dst.size = 4;
					break;
				}
				default:;
			}
			break;
		case 0x27:
			inst.type        = ShaderInstructionType::ImageSampleLz;
			inst.src[0].size = 3;
			inst.src[1].size = 8;
			inst.src[2].size = 4;
			switch (dmask) // NOLINT
			{
				case 0x7:
				{
					inst.format   = ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7;
					inst.dst.size = 3;
					break;
				}
				default:;
			}
			break;
		case 0x37:
			inst.type        = ShaderInstructionType::ImageSampleLzO;
			inst.src[0].size = 4;
			inst.src[1].size = 8;
			inst.src[2].size = 4;
			switch (dmask) // NOLINT
			{
				case 0x7:
				{
					inst.format   = ShaderInstructionFormat::Vdata3Vaddr4StSsDmask7;
					inst.dst.size = 3;
					break;
				}
				default:;
			}
			break;
		default: printf("%s", dst->DbgDump().C_Str()); EXIT("unknown mimg opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 "\n", opcode, pc);
	}

	if (inst.format == ShaderInstructionFormat::Unknown)
	{
		printf("%s", dst->DbgDump().C_Str());
		EXIT("unknown mimg format for opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 ", dmask: 0x%" PRIx32 "\n", opcode, pc, dmask);
	}

	dst->GetInstructions().Add(inst);

	return size;
}

KYTY_SHADER_PARSER(shader_parse_mtbuf)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	uint32_t opcode = (buffer[0] >> 16u) & 0x7u;
	uint32_t dfmt   = (buffer[0] >> 19u) & 0xfu;
	uint32_t nfmt   = (buffer[0] >> 23u) & 0x7u;
	uint32_t glc    = (buffer[0] >> 14u) & 0x1u;
	uint32_t idxen  = (buffer[0] >> 13u) & 0x1u;
	uint32_t offen  = (buffer[0] >> 12u) & 0x1u;
	uint32_t offset = (buffer[0] >> 0u) & 0xfffu;

	uint32_t soffset = (buffer[1] >> 24u) & 0xffu;
	uint32_t tfe     = (buffer[1] >> 23u) & 0x1u;
	uint32_t slc     = (buffer[1] >> 22u) & 0x1u;
	uint32_t srsrc   = (buffer[1] >> 16u) & 0x1fu;
	uint32_t vdata   = (buffer[1] >> 8u) & 0xffu;
	uint32_t vaddr   = (buffer[1] >> 0u) & 0xffu;

	EXIT_NOT_IMPLEMENTED(idxen == 0);
	// EXIT_NOT_IMPLEMENTED(offen == 1);
	EXIT_NOT_IMPLEMENTED(offset != 0);
	EXIT_NOT_IMPLEMENTED(glc == 1);
	EXIT_NOT_IMPLEMENTED(slc == 1);
	EXIT_NOT_IMPLEMENTED(tfe == 1);
	// EXIT_NOT_IMPLEMENTED(dfmt != 14);
	// EXIT_NOT_IMPLEMENTED(nfmt != 7);

	if ((dfmt != 14 && dfmt != 4) || nfmt != 7)
	{
		EXIT("unknown format: dfmt = %d, nfmt = %d at addr 0x%08" PRIx32 " (hash0 = 0x%08" PRIx32 ")\n", dfmt, nfmt, pc, dst->GetHash0());
	}

	uint32_t size = 2;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.dst     = operand_parse(vdata + 256);
	inst.src_num = 3;
	inst.src[0]  = operand_parse(vaddr + 256);
	inst.src[1]  = operand_parse(srsrc * 4);
	inst.src[2]  = operand_parse(soffset);

	if (inst.src[2].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[2].constant.u = buffer[size];
		size++;
	}

	inst.src[1].size = 4;

	switch (opcode)
	{
		case 0x00:
			inst.type   = ShaderInstructionType::TBufferLoadFormatX;
			inst.format = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxenFloat1;
			EXIT_NOT_IMPLEMENTED(offen == 1);
			EXIT_NOT_IMPLEMENTED(!(dfmt == 4 && nfmt == 7));
			break;

		case 0x03:
			inst.type   = ShaderInstructionType::TBufferLoadFormatXyzw;
			inst.format = (offen == 1 ? ShaderInstructionFormat::Vdata4Vaddr2SvSoffsOffenIdxenFloat4
			                          : ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxenFloat4);
			inst.src[0].size += static_cast<int>(offen);
			inst.dst.size = 4;
			EXIT_NOT_IMPLEMENTED(!(dfmt == 14 && nfmt == 7));
			break;
		default: printf("%s", dst->DbgDump().C_Str()); EXIT("unknown mtbuf opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 "\n", opcode, pc);
	}

	dst->GetInstructions().Add(inst);

	return size;
}

KYTY_SHADER_PARSER(shader_parse_vintrp)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	uint32_t opcode = (buffer[0] >> 16u) & 0x3u;
	uint32_t vdst   = (buffer[0] >> 18u) & 0xffu;
	uint32_t attr   = (buffer[0] >> 10u) & 0x3fu;
	uint32_t chan   = (buffer[0] >> 8u) & 0x3u;
	uint32_t vsrc   = (buffer[0] >> 0u) & 0xffu;

	ShaderInstruction inst;
	inst.pc                = pc;
	inst.src[0]            = operand_parse(vsrc + 256);
	inst.dst               = operand_parse(vdst + 256);
	inst.src[1].type       = ShaderOperandType::IntegerInlineConstant;
	inst.src[1].constant.u = attr;
	inst.src[2].type       = ShaderOperandType::IntegerInlineConstant;
	inst.src[2].constant.u = chan;
	inst.src_num           = 3;

	switch (opcode)
	{
		case 0x0:
			inst.type   = ShaderInstructionType::VInterpP1F32;
			inst.format = ShaderInstructionFormat::VdstVsrcAttrChan;
			break;
		case 0x1:
			inst.type   = ShaderInstructionType::VInterpP2F32;
			inst.format = ShaderInstructionFormat::VdstVsrcAttrChan;
			break;
		default: printf("%s", dst->DbgDump().C_Str()); EXIT("unknown vintrp opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 "\n", opcode, pc);
	}

	dst->GetInstructions().Add(inst);

	return 1;
}

KYTY_SHADER_PARSER(shader_parse)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer != nullptr);

	auto type = dst->GetType();

	dst->GetInstructions().Clear();
	dst->GetLabels().Clear();

	const auto* ptr = src + pc / 4;
	for (;;)
	{
		auto instruction = ptr[0];
		auto pc          = 4 * static_cast<uint32_t>(ptr - src);

		if ((instruction & 0xF8000000u) == 0xC0000000)
		{
			ptr += shader_parse_smrd(pc, src, ptr, dst);
		} else if ((instruction & 0xC0000000u) == 0x80000000)
		{
			ptr += shader_parse_sop2(pc, src, ptr, dst);
		} else if ((instruction & 0xFC000000u) == 0xE0000000)
		{
			ptr += shader_parse_mubuf(pc, src, ptr, dst);
		} else if ((instruction & 0xFC000000u) == 0xE8000000)
		{
			ptr += shader_parse_mtbuf(pc, src, ptr, dst);
		} else if ((instruction & 0x80000000u) == 0x00000000)
		{
			ptr += shader_parse_vop2(pc, src, ptr, dst);
		} else if ((instruction & 0xFC000000u) == 0xD0000000)
		{
			ptr += shader_parse_vop3(pc, src, ptr, dst);
		} else if ((instruction & 0xFC000000u) == 0xF8000000)
		{
			ptr += shader_parse_exp(pc, src, ptr, dst);
		} else if ((instruction & 0xFC000000u) == 0xC8000000)
		{
			ptr += shader_parse_vintrp(pc, src, ptr, dst);
		} else if ((instruction & 0xFC000000u) == 0xF0000000)
		{
			ptr += shader_parse_mimg(pc, src, ptr, dst);
		} else if ((instruction & 0xFC000000u) == 0xD8000000)
		{
			ptr += shader_parse_ds(pc, src, ptr, dst);
		} else
		{
			printf("%s", dst->DbgDump().C_Str());
			EXIT("unknown code 0x%08" PRIx32 " at addr 0x%08" PRIx32 "\n", ptr[0], pc);
		}

		if ((instruction == 0xBF810000 && (type == ShaderType::Vertex || type == ShaderType::Pixel || type == ShaderType::Compute) &&
		     !dst->GetLabels().Contains(4 * static_cast<uint32_t>(ptr - src), [](auto label, auto pc) { return label.GetDst() == pc; })) ||
		    (instruction == 0xBE802000 && type == ShaderType::Fetch))
		{
			break;
		}
	}

	return ptr - src;
}

static void vs_print(const char* func, const HW::VsStageRegisters& vs)
{
	printf("%s\n", func);

	printf("\t GetGpuAddress()           = 0x%016" PRIx64 "\n", vs.GetGpuAddress());
	printf("\t GetStreamoutEnabled()     = %s\n", vs.GetStreamoutEnabled() ? "true" : "false");
	printf("\t GetSgprCount()            = 0x%08" PRIx32 "\n", vs.GetSgprCount());
	printf("\t GetInputComponentsCount() = 0x%08" PRIx32 "\n", vs.GetInputComponentsCount());
	printf("\t GetUnknown1()             = 0x%08" PRIx32 "\n", vs.GetUnknown1());
	printf("\t GetUnknown2()             = 0x%08" PRIx32 "\n", vs.GetUnknown2());
	printf("\t m_spiVsOutConfig          = 0x%08" PRIx32 "\n", vs.m_spiVsOutConfig);
	printf("\t m_spiShaderPosFormat      = 0x%08" PRIx32 "\n", vs.m_spiShaderPosFormat);
	printf("\t m_paClVsOutCntl           = 0x%08" PRIx32 "\n", vs.m_paClVsOutCntl);
}

static void ps_print(const char* func, const HW::PsStageRegisters& ps)
{
	printf("%s\n", func);

	// printf("\t GetGpuAddress()               = 0x%016" PRIx64 "\n", ps.GetGpuAddress());
	// printf("\t m_spiShaderPgmRsrc1Ps         = 0x%08" PRIx32 "\n", ps.m_spiShaderPgmRsrc1Ps);
	// printf("\t m_spiShaderPgmRsrc2Ps         = 0x%08" PRIx32 "\n", ps.m_spiShaderPgmRsrc2Ps);
	// printf("\t GetTargetOutputMode(0)        = 0x%08" PRIx32 "\n", ps.GetTargetOutputMode(0));
	printf("\t data_addr                   = 0x%016" PRIx64 "\n", ps.data_addr);
	printf("\t conservative_z_export_value = 0x%08" PRIx32 "\n", ps.conservative_z_export_value);
	printf("\t shader_z_behavior           = 0x%08" PRIx32 "\n", ps.shader_z_behavior);
	printf("\t shader_kill_enable          = %s\n", ps.shader_kill_enable ? "true" : "false");
	printf("\t shader_z_export_enable      = %s\n", ps.shader_z_export_enable ? "true" : "false");
	printf("\t shader_execute_on_noop      = %s\n", ps.shader_execute_on_noop ? "true" : "false");
	printf("\t vgprs                       = 0x%02" PRIx8 "\n", ps.vgprs);
	printf("\t sgprs                       = 0x%02" PRIx8 "\n", ps.sgprs);
	printf("\t scratch_en                  = 0x%02" PRIx8 "\n", ps.scratch_en);
	printf("\t user_sgpr                   = 0x%02" PRIx8 "\n", ps.user_sgpr);
	printf("\t wave_cnt_en                 = 0x%02" PRIx8 "\n", ps.wave_cnt_en);
	printf("\t shader_z_format             = 0x%08" PRIx32 "\n", ps.shader_z_format);
	printf("\t target_output_mode[0]       = 0x%02" PRIx8 "\n", ps.target_output_mode[0]);
	printf("\t ps_input_ena                = 0x%08" PRIx32 "\n", ps.ps_input_ena);
	printf("\t ps_input_addr               = 0x%08" PRIx32 "\n", ps.ps_input_addr);
	printf("\t ps_in_control               = 0x%08" PRIx32 "\n", ps.ps_in_control);
	printf("\t baryc_cntl                  = 0x%08" PRIx32 "\n", ps.baryc_cntl);
	printf("\t m_cbShaderMask              = 0x%08" PRIx32 "\n", ps.m_cbShaderMask);
}

static void cs_print(const char* func, const HW::CsStageRegisters& cs)
{
	printf("%s\n", func);

	//	printf("\t GetGpuAddress()        = 0x%016" PRIx64 "\n", cs.GetGpuAddress());
	//	printf("\t m_computePgmLo         = 0x%08" PRIx32 "\n", cs.m_computePgmLo);
	//	printf("\t m_computePgmHi         = 0x%08" PRIx32 "\n", cs.m_computePgmHi);
	//	printf("\t m_computePgmRsrc1      = 0x%08" PRIx32 "\n", cs.m_computePgmRsrc1);
	//	printf("\t m_computePgmRsrc2      = 0x%08" PRIx32 "\n", cs.m_computePgmRsrc2);
	//	printf("\t m_computeNumThreadX    = 0x%08" PRIx32 "\n", cs.m_computeNumThreadX);
	//	printf("\t m_computeNumThreadY    = 0x%08" PRIx32 "\n", cs.m_computeNumThreadY);
	//	printf("\t m_computeNumThreadZ    = 0x%08" PRIx32 "\n", cs.m_computeNumThreadZ);
	printf("\t data_addr      = 0x%016" PRIx64 "\n", cs.data_addr);
	printf("\t num_thread_x   = 0x%08" PRIx32 "\n", cs.num_thread_x);
	printf("\t num_thread_y   = 0x%08" PRIx32 "\n", cs.num_thread_y);
	printf("\t num_thread_z   = 0x%08" PRIx32 "\n", cs.num_thread_z);
	printf("\t vgprs          = 0x%02" PRIx8 "\n", cs.vgprs);
	printf("\t sgprs          = 0x%02" PRIx8 "\n", cs.sgprs);
	printf("\t bulky          = 0x%02" PRIx8 "\n", cs.bulky);
	printf("\t scratch_en     = 0x%02" PRIx8 "\n", cs.scratch_en);
	printf("\t user_sgpr      = 0x%02" PRIx8 "\n", cs.user_sgpr);
	printf("\t tgid_x_en      = 0x%02" PRIx8 "\n", cs.tgid_x_en);
	printf("\t tgid_y_en      = 0x%02" PRIx8 "\n", cs.tgid_y_en);
	printf("\t tgid_z_en      = 0x%02" PRIx8 "\n", cs.tgid_z_en);
	printf("\t tg_size_en     = 0x%02" PRIx8 "\n", cs.tg_size_en);
	printf("\t tidig_comp_cnt = 0x%02" PRIx8 "\n", cs.tidig_comp_cnt);
	printf("\t lds_size       = 0x%02" PRIx8 "\n", cs.lds_size);
}

static void bi_print(const char* func, const ShaderBinaryInfo& bi)
{
	printf("%s\n", func);

	printf("\t signature                  = %.7s\n", bi.signature);
	printf("\t version                    = 0x%02" PRIx8 "\n", bi.version);
	printf("\t pssl_or_cg                 = 0x%08" PRIx32 "\n", static_cast<uint32_t>(bi.pssl_or_cg));
	printf("\t cached                     = 0x%08" PRIx32 "\n", static_cast<uint32_t>(bi.cached));
	printf("\t type                       = 0x%08" PRIx32 "\n", static_cast<uint32_t>(bi.type));
	printf("\t source_type                = 0x%08" PRIx32 "\n", static_cast<uint32_t>(bi.source_type));
	printf("\t length                     = 0x%08" PRIx32 "\n", static_cast<uint32_t>(bi.length));
	printf("\t chunk_usage_base_offset_dw = 0x%02" PRIx8 "\n", bi.chunk_usage_base_offset_dw);
	printf("\t num_input_usage_slots      = 0x%02" PRIx8 "\n", bi.num_input_usage_slots);
	printf("\t is_srt                     = 0x%02" PRIx8 "\n", bi.is_srt);
	printf("\t is_srt_used_info_valid     = 0x%02" PRIx8 "\n", bi.is_srt_used_info_valid);
	printf("\t is_extended_usage_info     = 0x%02" PRIx8 "\n", bi.is_extended_usage_info);
	printf("\t reserved2                  = 0x%02" PRIx8 "\n", bi.reserved2);
	printf("\t reserved3                  = 0x%02" PRIx8 "\n", bi.reserved3);
	printf("\t hash0                      = 0x%08" PRIx32 "\n", bi.hash0);
	printf("\t hash1                      = 0x%08" PRIx32 "\n", bi.hash1);
	printf("\t crc32                      = 0x%08" PRIx32 "\n", bi.crc32);
}

static void vs_check(const HW::VsStageRegisters& vs)
{
	EXIT_NOT_IMPLEMENTED(vs.GetStreamoutEnabled() != false);
	// EXIT_NOT_IMPLEMENTED(vs.GetSgprCount() != 0x00000000);
	// EXIT_NOT_IMPLEMENTED(vs.GetInputComponentsCount() != 0x00000003);
	// EXIT_NOT_IMPLEMENTED(vs.GetUnknown1() != 0x002c0000);
	// EXIT_NOT_IMPLEMENTED(vs.GetUnknown2() != 0x00000000);
	// EXIT_NOT_IMPLEMENTED(vs.m_spiVsOutConfig != 0x00000000);
	EXIT_NOT_IMPLEMENTED(vs.m_spiShaderPosFormat != 0x00000004);
	EXIT_NOT_IMPLEMENTED(vs.m_paClVsOutCntl != 0x00000000);
}

static void ps_check(const HW::PsStageRegisters& ps)
{
	EXIT_NOT_IMPLEMENTED(ps.target_output_mode[0] != 4 && ps.target_output_mode[0] != 9);
	EXIT_NOT_IMPLEMENTED(ps.conservative_z_export_value != 0x00000000);
	EXIT_NOT_IMPLEMENTED(ps.shader_z_behavior != 0x00000001 && ps.shader_z_behavior != 0x00000000);
	// EXIT_NOT_IMPLEMENTED(ps.shader_kill_enable != false);
	EXIT_NOT_IMPLEMENTED(ps.shader_z_export_enable != false);
	// EXIT_NOT_IMPLEMENTED(ps.shader_execute_on_noop != false);
	// EXIT_NOT_IMPLEMENTED(ps.m_spiShaderPgmRsrc1Ps != 0x002c0000);
	// EXIT_NOT_IMPLEMENTED(ps.m_spiShaderPgmRsrc2Ps != 0x00000000);
	// EXIT_NOT_IMPLEMENTED(ps.vgprs != 0x00 && ps.vgprs != 0x01);
	// EXIT_NOT_IMPLEMENTED(ps.sgprs != 0x00 && ps.sgprs != 0x01);
	EXIT_NOT_IMPLEMENTED(ps.scratch_en != 0);
	// EXIT_NOT_IMPLEMENTED(ps.user_sgpr != 0 && ps.user_sgpr != 4 && ps.user_sgpr != 12);
	EXIT_NOT_IMPLEMENTED(ps.wave_cnt_en != 0);
	EXIT_NOT_IMPLEMENTED(ps.shader_z_format != 0x00000000);
	EXIT_NOT_IMPLEMENTED(ps.ps_input_ena != 0x00000002 && ps.ps_input_ena != 0x00000302);
	EXIT_NOT_IMPLEMENTED(ps.ps_input_addr != 0x00000002 && ps.ps_input_addr != 0x00000302);
	// EXIT_NOT_IMPLEMENTED(ps.m_spiPsInControl != 0x00000000);
	EXIT_NOT_IMPLEMENTED(ps.baryc_cntl != 0x00000000);
	EXIT_NOT_IMPLEMENTED(ps.m_cbShaderMask != 0x0000000f);
}

static void cs_check(const HW::CsStageRegisters& cs)
{
	// EXIT_NOT_IMPLEMENTED(cs.num_thread_x != 0x00000040);
	// EXIT_NOT_IMPLEMENTED(cs.num_thread_y != 0x00000001);
	// EXIT_NOT_IMPLEMENTED(cs.num_thread_z != 0x00000001);
	// EXIT_NOT_IMPLEMENTED(cs.vgprs != 0x00 && cs.vgprs != 0x01);
	// EXIT_NOT_IMPLEMENTED(cs.sgprs != 0x01 && cs.sgprs != 0x02);
	EXIT_NOT_IMPLEMENTED(cs.bulky != 0x00);
	EXIT_NOT_IMPLEMENTED(cs.scratch_en != 0x00);
	// EXIT_NOT_IMPLEMENTED(cs.user_sgpr != 0x0c);
	EXIT_NOT_IMPLEMENTED(cs.tgid_x_en != 0x01);
	// EXIT_NOT_IMPLEMENTED(cs.tgid_y_en != 0x00);
	// EXIT_NOT_IMPLEMENTED(cs.tgid_z_en != 0x00);
	EXIT_NOT_IMPLEMENTED(cs.tg_size_en != 0x00);
	EXIT_NOT_IMPLEMENTED(cs.tidig_comp_cnt > 2);
	EXIT_NOT_IMPLEMENTED(cs.lds_size != 0x00);

	//	EXIT_NOT_IMPLEMENTED(cs.m_computePgmRsrc1 != 0x002c0040);
	//	EXIT_NOT_IMPLEMENTED(cs.m_computePgmRsrc2 != 0x00000098);
	//	EXIT_NOT_IMPLEMENTED(cs.m_computeNumThreadX != 0x00000040);
	//	EXIT_NOT_IMPLEMENTED(cs.m_computeNumThreadY != 0x00000001);
	//	EXIT_NOT_IMPLEMENTED(cs.m_computeNumThreadZ != 0x00000001);
}

static bool SpirvDisassemble(const uint32_t* src_binary, size_t src_binary_size, String* dst_disassembly)
{
	if (dst_disassembly != nullptr)
	{
		spvtools::SpirvTools core(SPV_ENV_VULKAN_1_2);

		std::string disassembly;
		if (!core.Disassemble(src_binary, src_binary_size, &disassembly,
		                      static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
		                          static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
		                          static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
		                          static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
		                          static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR)))
		{
			*dst_disassembly = disassembly.c_str();

			printf("Disassemble failed\n");
			return false;
		}

		*dst_disassembly = disassembly.c_str();
	}
	return true;
}

static bool SpirvToGlsl(const uint32_t* /*src_binary*/, size_t /*src_binary_size*/, String* /*dst_code*/)
{
	//	if (dst_code != nullptr)
	//	{
	//		spirv_cross::CompilerGLSL glsl(src_binary, src_binary_size);
	//
	//		std::string source = glsl.compile();
	//
	//		*dst_code = source.c_str();
	//	}
	return true;
}

static bool SpirvRun(const String& src, Vector<uint32_t>* dst, String* err_msg)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(err_msg == nullptr);

	spvtools::SpirvTools core(SPV_ENV_VULKAN_1_2);
	spvtools::Optimizer  opt(SPV_ENV_VULKAN_1_2);

	spv_position_t error_position {};
	String         error_msg;

	auto print_msg_to_stderr = [&error_position, &error_msg](spv_message_level_t /* level */, const char* /*source*/,
	                                                         [[maybe_unused]] const spv_position_t& position, const char* m)
	{
		// printf("%s\n", source);
		error_msg = String::FromPrintf("%d: %d (%d) %s", static_cast<int>(position.line), static_cast<int>(position.column),
		                               static_cast<int>(position.index), m);
		printf(FG_BRIGHT_RED "error: %s\n" FG_DEFAULT, error_msg.C_Str());
		error_position = position;
	};
	core.SetMessageConsumer(print_msg_to_stderr);
	opt.SetMessageConsumer(print_msg_to_stderr);

	String::Utf8 src_utf8 = src.utf8_str();

	dst->Clear();

	std::vector<uint32_t> spirv;
	if (!core.Assemble(src_utf8.GetDataConst(), src_utf8.Size(), &spirv))
	{
		printf("Assemble failed at:\n%s\n", src.Mid(src.FindIndex(U'\n', error_position.index - 100), 200).C_Str());
		*err_msg = String::FromPrintf("Assemble failed at:\n%s\n", src.Mid(src.FindIndex(U'\n', error_position.index - 100), 200).C_Str());
		return false;
	}

	if (Config::ShaderValidationEnabled() && !core.Validate(spirv))
	{
		String disassembly;
		SpirvDisassemble(spirv.data(), spirv.size(), &disassembly);
		printf("%s\n", disassembly.C_Str());
		printf("Validate failed\n");
		*err_msg = String::FromPrintf("%s\n\nValidate failed:\n%s\n", Log::RemoveColors(disassembly).C_Str(), error_msg.C_Str());
		return false;
	}

	bool optimize = true;
	switch (Config::GetShaderOptimizationType())
	{
		case Config::ShaderOptimizationType::Performance: opt.RegisterPerformancePasses(); break;
		case Config::ShaderOptimizationType::Size: opt.RegisterSizePasses(); break;
		default: optimize = false; break;
	}

	if (optimize && !opt.Run(spirv.data(), spirv.size(), &spirv))
	{
		printf("Optimize failed\n");
		*err_msg = String::FromPrintf("Optimize failed\n");
		return false;
	}

	dst->Add(spirv.data(), spirv.size());

	return true;
}

static const ShaderBinaryInfo* GetBinaryInfo(const uint32_t* code)
{
	EXIT_IF(code == nullptr);

	if (code[0] == 0xBEEB03FF)
	{
		return reinterpret_cast<const ShaderBinaryInfo*>(code + static_cast<size_t>(code[1] + 1) * 2);
	}

	return nullptr;
}

static ShaderUsageInfo GetUsageSlots(const uint32_t* code)
{
	EXIT_IF(code == nullptr);

	const auto* binary_info = GetBinaryInfo(code);

	EXIT_NOT_IMPLEMENTED(binary_info == nullptr);
	EXIT_NOT_IMPLEMENTED(binary_info->chunk_usage_base_offset_dw == 0);

	ShaderUsageInfo ret;

	ret.usage_masks = (binary_info->chunk_usage_base_offset_dw == 0
	                       ? nullptr
	                       : reinterpret_cast<const uint32_t*>(binary_info) - binary_info->chunk_usage_base_offset_dw);
	ret.slots_num   = binary_info->num_input_usage_slots;
	ret.slots       = (ret.slots_num == 0 ? nullptr : reinterpret_cast<const ShaderUsageSlot*>(ret.usage_masks) - ret.slots_num);

	return ret;
}

static void ShaderDetectBuffers(ShaderVertexInputInfo* info)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr);

	info->buffers_num = 0;

	for (int ri = 0; ri < info->resources_num; ri++)
	{
		const auto& r = info->resources[ri];

		bool merged = false;
		for (int bi = 0; bi < info->buffers_num; bi++)
		{
			auto& b = info->buffers[bi];

			uint64_t stride = b.stride;

			if (stride == r.Stride())
			{
				uint64_t rbase   = r.Base();
				uint64_t base    = std::min(rbase, b.addr);
				uint64_t offset1 = rbase - base;
				uint64_t offset2 = b.addr - base;

				if (offset1 < stride && offset2 < stride)
				{
					EXIT_NOT_IMPLEMENTED(b.num_records != r.NumRecords());
					b.addr = base;
					EXIT_NOT_IMPLEMENTED(b.attr_num >= ShaderVertexInputBuffer::ATTR_MAX);
					b.attr_indices[b.attr_num++] = ri;
					merged                       = true;
					break;
				}
			}
		}

		if (!merged)
		{
			EXIT_NOT_IMPLEMENTED(info->buffers_num >= ShaderVertexInputInfo::RES_MAX);
			int bi                            = info->buffers_num++;
			info->buffers[bi].addr            = r.Base();
			info->buffers[bi].stride          = r.Stride();
			info->buffers[bi].num_records     = r.NumRecords();
			info->buffers[bi].attr_num        = 1;
			info->buffers[bi].attr_indices[0] = ri;
		}
	}

	for (int bi = 0; bi < info->buffers_num; bi++)
	{
		auto& b = info->buffers[bi];
		for (int ri = 0; ri < b.attr_num; ri++)
		{
			b.attr_offsets[ri] = info->resources[b.attr_indices[ri]].Base() - b.addr;
		}
	}
}

static void ShaderParseFetch(ShaderVertexInputInfo* info, const uint32_t* fetch, const uint32_t* buffer)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr || fetch == nullptr || buffer == nullptr);

	KYTY_PROFILER_BLOCK("ShaderParseFetch::parse_code");

	ShaderCode code;
	code.SetType(ShaderType::Fetch);
	shader_parse(0, fetch, nullptr, &code);

	KYTY_PROFILER_END_BLOCK;

	// printf("%s", code.DbgDump().C_Str());

	KYTY_PROFILER_BLOCK("ShaderParseFetch::check_insts");

	const auto& insts = code.GetInstructions();
	uint32_t    size  = insts.Size();
	// int         temp_register = 0;
	uint32_t temp_value[104] = {0};
	int      s_num           = 0;
	int      v_num           = 0;

	for (uint32_t i = 0; i < size; i++)
	{
		const auto& inst = insts.At(i);

		if (inst.type == ShaderInstructionType::SLoadDwordx4)
		{
			EXIT_NOT_IMPLEMENTED(inst.src[1].type != ShaderOperandType::LiteralConstant || (inst.src[1].constant.u & 3u) != 0);
			EXIT_NOT_IMPLEMENTED(inst.src[0].type != ShaderOperandType::Sgpr || inst.src[0].register_id != 2);
			EXIT_NOT_IMPLEMENTED(inst.dst.type != ShaderOperandType::Sgpr);

			uint32_t index    = inst.src[1].constant.u >> 2u;
			int      t        = inst.dst.register_id;
			temp_value[t + 0] = buffer[index + 0];
			temp_value[t + 1] = buffer[index + 1];
			temp_value[t + 2] = buffer[index + 2];
			temp_value[t + 3] = buffer[index + 3];

			s_num++;
		}

		bool load_inst     = true;
		int  registers_num = 0;
		switch (inst.type)
		{
			case ShaderInstructionType::BufferLoadFormatX: registers_num = 1; break;
			case ShaderInstructionType::BufferLoadFormatXy: registers_num = 2; break;
			case ShaderInstructionType::BufferLoadFormatXyz: registers_num = 3; break;
			case ShaderInstructionType::BufferLoadFormatXyzw: registers_num = 4; break;
			default: load_inst = false;
		}

		if (load_inst && registers_num > 0)
		{
			// EXIT_NOT_IMPLEMENTED(!(i >= 2 && insts.At(i - 1).type == ShaderInstructionType::SWaitcnt &&
			//                       insts.At(i - 2).type == ShaderInstructionType::SLoadDwordx4));
			EXIT_NOT_IMPLEMENTED(inst.dst.type != ShaderOperandType::Vgpr);
			EXIT_NOT_IMPLEMENTED(inst.src[0].type != ShaderOperandType::Vgpr || inst.src[0].register_id != 0);
			EXIT_NOT_IMPLEMENTED(inst.src[1].type != ShaderOperandType::Sgpr);
			EXIT_NOT_IMPLEMENTED(inst.src[2].type != ShaderOperandType::IntegerInlineConstant || inst.src[2].constant.i != 0);

			EXIT_NOT_IMPLEMENTED(info->resources_num >= ShaderVertexInputInfo::RES_MAX);

			int t = inst.src[1].register_id;

			auto& r           = info->resources[info->resources_num];
			auto& rd          = info->resources_dst[info->resources_num];
			rd.register_start = inst.dst.register_id;
			rd.registers_num  = registers_num;
			r.fields[0]       = temp_value[t + 0];
			r.fields[1]       = temp_value[t + 1];
			r.fields[2]       = temp_value[t + 2];
			r.fields[3]       = temp_value[t + 3];

			info->resources_num++;

			v_num++;
		}
	}

	KYTY_PROFILER_END_BLOCK;

	EXIT_NOT_IMPLEMENTED(s_num != v_num);
}

static void ShaderGetStorageBuffer(ShaderStorageResources* info, int start_index, int slot, ShaderStorageUsage usage,
                                   const HW::UserSgprInfo& user_sgpr, const uint32_t* extended_buffer)
{
	EXIT_IF(info == nullptr);

	EXIT_NOT_IMPLEMENTED(info->buffers_num < 0 || info->buffers_num >= ShaderStorageResources::BUFFERS_MAX);

	int  index    = info->buffers_num;
	bool extended = (extended_buffer != nullptr);

	EXIT_NOT_IMPLEMENTED(!extended && start_index >= 16);
	EXIT_NOT_IMPLEMENTED(extended && !(start_index >= 16));

	info->start_register[index] = start_index;
	info->slots[index]          = slot;
	info->usages[index]         = usage;
	info->extended[index]       = extended;
	// info->extended_index[index] = extended_index;

	if (!extended)
	{
		for (int j = 0; j < 4; j++)
		{
			auto type = user_sgpr.type[start_index + j];
			EXIT_NOT_IMPLEMENTED(type != HW::UserSgprType::Vsharp && type != HW::UserSgprType::Region);
		}
	}

	info->buffers[index].fields[0] = (extended ? extended_buffer[start_index - 16 + 0] : user_sgpr.value[start_index + 0]);
	info->buffers[index].fields[1] = (extended ? extended_buffer[start_index - 16 + 1] : user_sgpr.value[start_index + 1]);
	info->buffers[index].fields[2] = (extended ? extended_buffer[start_index - 16 + 2] : user_sgpr.value[start_index + 2]);
	info->buffers[index].fields[3] = (extended ? extended_buffer[start_index - 16 + 3] : user_sgpr.value[start_index + 3]);

	info->buffers_num++;
}

static void ShaderGetTextureBuffer(ShaderTextureResources* info, int start_index, int slot, ShaderTextureUsage usage,
                                   const HW::UserSgprInfo& user_sgpr, const uint32_t* extended_buffer)
{
	EXIT_IF(info == nullptr);

	EXIT_NOT_IMPLEMENTED(info->textures_num < 0 || info->textures_num >= ShaderTextureResources::RES_MAX);
	// EXIT_NOT_IMPLEMENTED(info->textures_num != slot);

	int  index    = info->textures_num;
	bool extended = (extended_buffer != nullptr);

	EXIT_NOT_IMPLEMENTED(!extended && start_index >= 16);
	EXIT_NOT_IMPLEMENTED(extended && !(start_index >= 16));

	info->desc[index].start_register = start_index;
	info->desc[index].extended       = extended;
	info->desc[index].slot           = slot;
	info->desc[index].usage          = usage;

	EXIT_IF(usage == ShaderTextureUsage::Unknown);

	if (usage == ShaderTextureUsage::ReadWrite)
	{
		info->textures2d_storage_num++;
		info->desc[index].textures2d_without_sampler = true;
	} else
	{
		info->textures2d_sampled_num++;
		info->desc[index].textures2d_without_sampler = false;
	}

	if (!extended)
	{
		for (int j = 0; j < 8; j++)
		{
			auto type = user_sgpr.type[start_index + j];
			EXIT_NOT_IMPLEMENTED(type != HW::UserSgprType::Vsharp && type != HW::UserSgprType::Region);
		}
	}

	info->desc[index].texture.fields[0] = (extended ? extended_buffer[start_index - 16 + 0] : user_sgpr.value[start_index + 0]);
	info->desc[index].texture.fields[1] = (extended ? extended_buffer[start_index - 16 + 1] : user_sgpr.value[start_index + 1]);
	info->desc[index].texture.fields[2] = (extended ? extended_buffer[start_index - 16 + 2] : user_sgpr.value[start_index + 2]);
	info->desc[index].texture.fields[3] = (extended ? extended_buffer[start_index - 16 + 3] : user_sgpr.value[start_index + 3]);
	info->desc[index].texture.fields[4] = (extended ? extended_buffer[start_index - 16 + 4] : user_sgpr.value[start_index + 4]);
	info->desc[index].texture.fields[5] = (extended ? extended_buffer[start_index - 16 + 5] : user_sgpr.value[start_index + 5]);
	info->desc[index].texture.fields[6] = (extended ? extended_buffer[start_index - 16 + 6] : user_sgpr.value[start_index + 6]);
	info->desc[index].texture.fields[7] = (extended ? extended_buffer[start_index - 16 + 7] : user_sgpr.value[start_index + 7]);

	info->textures_num++;
}

static void ShaderGetSampler(ShaderSamplerResources* info, int start_index, int slot, const HW::UserSgprInfo& user_sgpr,
                             const uint32_t* extended_buffer)
{
	EXIT_IF(info == nullptr);

	EXIT_NOT_IMPLEMENTED(info->samplers_num < 0 || info->samplers_num >= ShaderSamplerResources::RES_MAX);
	// EXIT_NOT_IMPLEMENTED(info->samplers_num != slot);

	int  index    = info->samplers_num;
	bool extended = (extended_buffer != nullptr);

	EXIT_NOT_IMPLEMENTED(!extended && start_index >= 16);
	EXIT_NOT_IMPLEMENTED(extended && !(start_index >= 16));

	info->start_register[index] = start_index;
	info->extended[index]       = extended;
	info->slots[index]          = slot;

	if (!extended)
	{
		for (int j = 0; j < 4; j++)
		{
			auto type = user_sgpr.type[start_index + j];
			EXIT_NOT_IMPLEMENTED(type != HW::UserSgprType::Vsharp && type != HW::UserSgprType::Region);
		}
	}

	info->samplers[index].fields[0] = (extended ? extended_buffer[start_index - 16 + 0] : user_sgpr.value[start_index + 0]);
	info->samplers[index].fields[1] = (extended ? extended_buffer[start_index - 16 + 1] : user_sgpr.value[start_index + 1]);
	info->samplers[index].fields[2] = (extended ? extended_buffer[start_index - 16 + 2] : user_sgpr.value[start_index + 2]);
	info->samplers[index].fields[3] = (extended ? extended_buffer[start_index - 16 + 3] : user_sgpr.value[start_index + 3]);

	info->samplers_num++;
}

static void ShaderGetGdsPointer(ShaderGdsResources* info, int start_index, int slot, const HW::UserSgprInfo& user_sgpr,
                                const uint32_t* extended_buffer)
{
	EXIT_IF(info == nullptr);

	EXIT_NOT_IMPLEMENTED(info->pointers_num < 0 || info->pointers_num >= ShaderGdsResources::POINTERS_MAX);
	// EXIT_NOT_IMPLEMENTED(info->pointers_num != slot);

	int  index    = info->pointers_num;
	bool extended = (extended_buffer != nullptr);

	EXIT_NOT_IMPLEMENTED(!extended && start_index >= 16);
	EXIT_NOT_IMPLEMENTED(extended && !(start_index >= 16));

	info->start_register[index] = start_index;
	info->extended[index]       = extended;
	info->slots[index]          = slot;

	if (!extended)
	{
		auto type = user_sgpr.type[start_index];
		EXIT_NOT_IMPLEMENTED(type != HW::UserSgprType::Unknown);
	}

	info->pointers[index].field = (extended ? extended_buffer[start_index - 16] : user_sgpr.value[start_index]);

	info->pointers_num++;
}

void ShaderCalcBindingIndices(ShaderBindResources* bind)
{
	int binding_index = 0;

	bind->push_constant_size = 0;

	if (bind->storage_buffers.buffers_num > 0)
	{
		bind->storage_buffers.binding_index = binding_index++;
		bind->push_constant_size += bind->storage_buffers.buffers_num * 16;
	}

	if (bind->textures2D.textures_num > 0)
	{
		bind->textures2D.binding_sampled_index = binding_index++;
		bind->textures2D.binding_storage_index = binding_index++;

		bind->push_constant_size += bind->textures2D.textures_num * 32;
	}

	if (bind->samplers.samplers_num > 0)
	{
		bind->samplers.binding_index = binding_index++;
		bind->push_constant_size += bind->samplers.samplers_num * 16;
	}

	if (bind->gds_pointers.pointers_num > 0)
	{
		bind->gds_pointers.binding_index = binding_index++;
		bind->push_constant_size += (((bind->gds_pointers.pointers_num - 1) / 4) + 1) * 16;
	}

	EXIT_IF((bind->push_constant_size % 16) != 0);
}

void ShaderGetInputInfoVS(const HW::VertexShaderInfo* regs, ShaderVertexInputInfo* info)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr || regs == nullptr);

	info->export_count              = static_cast<int>(1 + ((regs->vs_regs.m_spiVsOutConfig >> 1u) & 0x1Fu));
	info->bind.push_constant_offset = 0;
	info->bind.push_constant_size   = 0;
	info->bind.descriptor_set_slot  = 0;

	if (regs->vs_embedded)
	{
		return;
	}

	const auto* src = reinterpret_cast<const uint32_t*>(regs->vs_regs.GetGpuAddress());

	auto usages = GetUsageSlots(src);

	bool fetch             = false;
	int  fetch_reg         = 0;
	bool vertex_buffer     = false;
	int  vertex_buffer_reg = 0;

	KYTY_PROFILER_BLOCK("ShaderGetInputInfoVS::usages_cycle");

	for (int i = 0; i < usages.slots_num; i++)
	{
		const auto& usage = usages.slots[i];
		switch (usage.type)
		{
			case 0x02:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				ShaderGetStorageBuffer(&info->bind.storage_buffers, usage.start_register, usage.slot, ShaderStorageUsage::Constant,
				                       regs->vs_user_sgpr, nullptr);
				break;
			case 0x12:
				EXIT_NOT_IMPLEMENTED(usage.slot != 0);
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				fetch     = true;
				fetch_reg = usage.start_register;
				break;
			case 0x17:
				EXIT_NOT_IMPLEMENTED(usage.slot != 0);
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				vertex_buffer     = true;
				vertex_buffer_reg = usage.start_register;
				break;
			default: EXIT("unknown usage type: 0x%02" PRIx8 "\n", usage.type);
		}
	}

	KYTY_PROFILER_END_BLOCK;

	KYTY_PROFILER_BLOCK("ShaderGetInputInfoVS::parse_fetch");

	EXIT_NOT_IMPLEMENTED((fetch && !vertex_buffer) || (!fetch && vertex_buffer));

	if (fetch && vertex_buffer)
	{
		info->fetch = true;

		EXIT_NOT_IMPLEMENTED(fetch_reg >= 16 || vertex_buffer_reg >= 16);

		const auto* fetch = reinterpret_cast<const uint32_t*>(static_cast<uint64_t>(regs->vs_user_sgpr.value[fetch_reg]) |
		                                                      (static_cast<uint64_t>(regs->vs_user_sgpr.value[fetch_reg + 1]) << 32u));
		const auto* buffer =
		    reinterpret_cast<const uint32_t*>(static_cast<uint64_t>(regs->vs_user_sgpr.value[vertex_buffer_reg]) |
		                                      (static_cast<uint64_t>(regs->vs_user_sgpr.value[vertex_buffer_reg + 1]) << 32u));

		EXIT_NOT_IMPLEMENTED(fetch == nullptr || buffer == nullptr);

		ShaderParseFetch(info, fetch, buffer);
		ShaderDetectBuffers(info);
	}

	KYTY_PROFILER_END_BLOCK;

	KYTY_PROFILER_BLOCK("ShaderGetInputInfoVS::calc_binding");

	ShaderCalcBindingIndices(&info->bind);

	KYTY_PROFILER_END_BLOCK;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ShaderGetInputInfoPS(const HW::PixelShaderInfo* regs, const ShaderVertexInputInfo* vs_info, ShaderPixelInputInfo* ps_info)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(vs_info == nullptr);
	EXIT_IF(ps_info == nullptr);
	EXIT_IF(regs == nullptr);

	if (regs->ps_embedded)
	{
		return;
	}

	ps_info->input_num            = regs->ps_regs.ps_in_control;
	ps_info->ps_pos_xy            = (regs->ps_regs.ps_input_ena == 0x00000302 && regs->ps_regs.ps_input_addr == 0x00000302);
	ps_info->ps_pixel_kill_enable = regs->ps_regs.shader_kill_enable;
	ps_info->ps_early_z           = (regs->ps_regs.shader_z_behavior == 1);
	ps_info->ps_execute_on_noop   = regs->ps_regs.shader_execute_on_noop;

	for (uint32_t i = 0; i < ps_info->input_num; i++)
	{
		ps_info->interpolator_settings[i] = regs->ps_interpolator_settings[i];
	}

	ps_info->bind.descriptor_set_slot  = (vs_info->bind.storage_buffers.buffers_num > 0 ? 1 : 0);
	ps_info->bind.push_constant_offset = vs_info->bind.push_constant_offset + vs_info->bind.push_constant_size;
	ps_info->bind.push_constant_size   = 0;

	for (int i = 0; i < 8; i++)
	{
		ps_info->target_output_mode[i] = regs->ps_regs.target_output_mode[i];
	}

	const auto* src = reinterpret_cast<const uint32_t*>(regs->ps_regs.data_addr);

	auto usages = GetUsageSlots(src);

	uint32_t* extended_buffer = nullptr;

	for (int i = 0; i < usages.slots_num; i++)
	{
		const auto& usage = usages.slots[i];
		switch (usage.type)
		{
			case 0x00:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0 && usage.flags != 3);
				if (usage.flags == 0)
				{
					ShaderGetStorageBuffer(&ps_info->bind.storage_buffers, usage.start_register, usage.slot, ShaderStorageUsage::ReadOnly,
					                       regs->ps_user_sgpr, extended_buffer);
				} else if (usage.flags == 3)
				{
					ShaderGetTextureBuffer(&ps_info->bind.textures2D, usage.start_register, usage.slot, ShaderTextureUsage::ReadOnly,
					                       regs->ps_user_sgpr, extended_buffer);
					EXIT_NOT_IMPLEMENTED(ps_info->bind.textures2D.desc[ps_info->bind.textures2D.textures_num - 1].texture.Type() != 9);
				}
				break;
			case 0x01:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				ShaderGetSampler(&ps_info->bind.samplers, usage.start_register, usage.slot, regs->ps_user_sgpr, extended_buffer);
				break;
			case 0x02:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				ShaderGetStorageBuffer(&ps_info->bind.storage_buffers, usage.start_register, usage.slot, ShaderStorageUsage::Constant,
				                       regs->ps_user_sgpr, extended_buffer);
				break;
			case 0x04:
				EXIT_NOT_IMPLEMENTED(usage.flags != 3);
				if (usage.flags == 3)
				{
					ShaderGetTextureBuffer(&ps_info->bind.textures2D, usage.start_register, usage.slot, ShaderTextureUsage::ReadWrite,
					                       regs->ps_user_sgpr, extended_buffer);
					EXIT_NOT_IMPLEMENTED(ps_info->bind.textures2D.desc[ps_info->bind.textures2D.textures_num - 1].texture.Type() != 9);
				}
				break;
			case 0x1b:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				EXIT_NOT_IMPLEMENTED(usage.slot != 1);
				EXIT_NOT_IMPLEMENTED(ps_info->bind.extended.used);
				ps_info->bind.extended.used           = true;
				ps_info->bind.extended.slot           = usage.slot;
				ps_info->bind.extended.start_register = usage.start_register;
				ps_info->bind.extended.data.fields[0] = regs->ps_user_sgpr.value[usage.start_register];
				ps_info->bind.extended.data.fields[1] = regs->ps_user_sgpr.value[usage.start_register + 1];
				extended_buffer                       = reinterpret_cast<uint32_t*>(ps_info->bind.extended.data.Base());
				break;
			default: EXIT("unknown usage type: 0x%02" PRIx8 "\n", usage.type);
		}
	}

	ShaderCalcBindingIndices(&ps_info->bind);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ShaderGetInputInfoCS(const HW::ComputeShaderInfo* regs, ShaderComputeInputInfo* info)
{
	EXIT_IF(info == nullptr);
	EXIT_IF(regs == nullptr);

	info->threads_num[0] = regs->cs_regs.num_thread_x;
	info->threads_num[1] = regs->cs_regs.num_thread_y;
	info->threads_num[2] = regs->cs_regs.num_thread_z;
	info->group_id[0]    = regs->cs_regs.tgid_x_en != 0;
	info->group_id[1]    = regs->cs_regs.tgid_y_en != 0;
	info->group_id[2]    = regs->cs_regs.tgid_z_en != 0;
	info->thread_ids_num = regs->cs_regs.tidig_comp_cnt + 1;

	info->workgroup_register = regs->cs_regs.user_sgpr;

	info->bind.push_constant_offset = 0;
	info->bind.push_constant_size   = 0;
	info->bind.descriptor_set_slot  = 0;

	const auto* src = reinterpret_cast<const uint32_t*>(regs->cs_regs.data_addr);

	auto usages = GetUsageSlots(src);

	uint32_t* extended_buffer = nullptr;

	for (int i = 0; i < usages.slots_num; i++)
	{
		const auto& usage = usages.slots[i];
		switch (usage.type)
		{
			case 0x00:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0 && usage.flags != 3);
				if (usage.flags == 0)
				{
					ShaderGetStorageBuffer(&info->bind.storage_buffers, usage.start_register, usage.slot, ShaderStorageUsage::ReadOnly,
					                       regs->cs_user_sgpr, extended_buffer);
				} else if (usage.flags == 3)
				{
					ShaderGetTextureBuffer(&info->bind.textures2D, usage.start_register, usage.slot, ShaderTextureUsage::ReadOnly,
					                       regs->cs_user_sgpr, extended_buffer);
					EXIT_NOT_IMPLEMENTED(info->bind.textures2D.desc[info->bind.textures2D.textures_num - 1].texture.Type() != 9);
				}
				break;
			case 0x02:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				ShaderGetStorageBuffer(&info->bind.storage_buffers, usage.start_register, usage.slot, ShaderStorageUsage::Constant,
				                       regs->cs_user_sgpr, extended_buffer);
				break;
			case 0x04:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0 && usage.flags != 3);
				if (usage.flags == 0)
				{
					ShaderGetStorageBuffer(&info->bind.storage_buffers, usage.start_register, usage.slot, ShaderStorageUsage::ReadWrite,
					                       regs->cs_user_sgpr, extended_buffer);
				} else if (usage.flags == 3)
				{
					ShaderGetTextureBuffer(&info->bind.textures2D, usage.start_register, usage.slot, ShaderTextureUsage::ReadWrite,
					                       regs->cs_user_sgpr, extended_buffer);
					EXIT_NOT_IMPLEMENTED(info->bind.textures2D.desc[info->bind.textures2D.textures_num - 1].texture.Type() != 9);
				}
				break;
			case 0x07:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				ShaderGetGdsPointer(&info->bind.gds_pointers, usage.start_register, usage.slot, regs->cs_user_sgpr, extended_buffer);
				break;
			case 0x1b:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				EXIT_NOT_IMPLEMENTED(usage.slot != 1);
				EXIT_NOT_IMPLEMENTED(info->bind.extended.used);
				info->bind.extended.used           = true;
				info->bind.extended.slot           = usage.slot;
				info->bind.extended.start_register = usage.start_register;
				info->bind.extended.data.fields[0] = regs->cs_user_sgpr.value[usage.start_register];
				info->bind.extended.data.fields[1] = regs->cs_user_sgpr.value[usage.start_register + 1];
				extended_buffer                    = reinterpret_cast<uint32_t*>(info->bind.extended.data.Base());
				break;
			default: EXIT("unknown usage type: 0x%02" PRIx8 "\n", usage.type);
		}
	}

	ShaderCalcBindingIndices(&info->bind);
}

static void ShaderDbgDumpResources(const ShaderBindResources& bind)
{
	printf("\t descriptor_set_slot            = %u\n", bind.descriptor_set_slot);
	printf("\t push_constant_offset           = %u\n", bind.push_constant_offset);
	printf("\t push_constant_size             = %u\n", bind.push_constant_size);
	printf("\t storage_buffers.buffers_num    = %d\n", bind.storage_buffers.buffers_num);
	printf("\t storage_buffers.binding_index  = %d\n", bind.storage_buffers.binding_index);
	printf("\t textures.textures_num          = %d\n", bind.textures2D.textures_num);
	printf("\t textures.binding_sampled_index = %d\n", bind.textures2D.binding_sampled_index);
	printf("\t textures.binding_storage_index = %d\n", bind.textures2D.binding_storage_index);
	printf("\t samplers.samplers_num          = %d\n", bind.samplers.samplers_num);
	printf("\t samplers.binding_index         = %d\n", bind.samplers.binding_index);
	printf("\t gds_pointers.pointers_num      = %d\n", bind.gds_pointers.pointers_num);
	printf("\t gds_pointers.binding_index     = %d\n", bind.gds_pointers.binding_index);
	printf("\t extended.used                  = %s\n", (bind.extended.used ? "true" : "false"));
	printf("\t extended.slot                  = %d\n", bind.extended.slot);
	printf("\t extended.start_register        = %d\n", bind.extended.start_register);
	printf("\t extended.data.Base             = %" PRIx64 "\n", bind.extended.data.Base());

	for (int i = 0; i < bind.storage_buffers.buffers_num; i++)
	{
		const auto& r = bind.storage_buffers.buffers[i];

		printf("\t StorageBuffer %d\n", i);

		printf("\t\t fields           = %08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "\n", r.fields[3], r.fields[2], r.fields[1],
		       r.fields[0]);
		printf("\t\t Base()           = %" PRIx64 "\n", r.Base());
		printf("\t\t Stride()         = %" PRIu16 "\n", r.Stride());
		printf("\t\t SwizzleEnabled() = %s\n", r.SwizzleEnabled() ? "true" : "false");
		printf("\t\t NumRecords()     = %" PRIu32 "\n", r.NumRecords());
		printf("\t\t DstSelX()        = %" PRIu8 "\n", r.DstSelX());
		printf("\t\t DstSelY()        = %" PRIu8 "\n", r.DstSelY());
		printf("\t\t DstSelZ()        = %" PRIu8 "\n", r.DstSelZ());
		printf("\t\t DstSelW()        = %" PRIu8 "\n", r.DstSelW());
		printf("\t\t Nfmt()           = %" PRIu8 "\n", r.Nfmt());
		printf("\t\t Dfmt()           = %" PRIu8 "\n", r.Dfmt());
		printf("\t\t MemoryType()     = 0x%02" PRIx8 "\n", r.MemoryType());
		printf("\t\t AddTid()         = %s\n", r.AddTid() ? "true" : "false");
		printf("\t\t slot             = %d\n", bind.storage_buffers.slots[i]);
		printf("\t\t start_register   = %d\n", bind.storage_buffers.start_register[i]);
		printf("\t\t extended         = %s\n", (bind.storage_buffers.extended[i] ? "true" : "false"));
		printf("\t\t usage            = %s\n", Core::EnumName(bind.storage_buffers.usages[i]).C_Str());
	}

	for (int i = 0; i < bind.textures2D.textures_num; i++)
	{
		const auto& r = bind.textures2D.desc[i].texture;

		printf("\t Texture %d\n", i);

		printf("\t\t fields = %08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "\n",
		       r.fields[7], r.fields[6], r.fields[5], r.fields[4], r.fields[3], r.fields[2], r.fields[1], r.fields[0]);
		printf("\t\t Base()          = %" PRIx64 "\n", r.Base());
		printf("\t\t MemoryType()    = 0x%02" PRIx8 "\n", r.MemoryType());
		printf("\t\t MinLod()        = %" PRIu16 "\n", r.MinLod());
		printf("\t\t Dfmt()          = %" PRIu8 "\n", r.Dfmt());
		printf("\t\t Nfmt()          = %" PRIu8 "\n", r.Nfmt());
		printf("\t\t Width()         = %" PRIu16 "\n", r.Width());
		printf("\t\t Height()        = %" PRIu16 "\n", r.Height());
		printf("\t\t PerfMod()       = %" PRIu8 "\n", r.PerfMod());
		printf("\t\t Interlaced()    = %s\n", r.Interlaced() ? "true" : "false");
		printf("\t\t DstSelX()       = %" PRIu8 "\n", r.DstSelX());
		printf("\t\t DstSelY()       = %" PRIu8 "\n", r.DstSelY());
		printf("\t\t DstSelZ()       = %" PRIu8 "\n", r.DstSelZ());
		printf("\t\t DstSelW()       = %" PRIu8 "\n", r.DstSelW());
		printf("\t\t BaseLevel()     = %" PRIu8 "\n", r.BaseLevel());
		printf("\t\t LastLevel()     = %" PRIu8 "\n", r.LastLevel());
		printf("\t\t TilingIdx()     = %" PRIu8 "\n", r.TilingIdx());
		printf("\t\t Pow2Pad()       = %s\n", r.Pow2Pad() ? "true" : "false");
		printf("\t\t Type()          = %" PRIu8 "\n", r.Type());
		printf("\t\t Depth()         = %" PRIu16 "\n", r.Depth());
		printf("\t\t Pitch()         = %" PRIu16 "\n", r.Pitch());
		printf("\t\t BaseArray()     = %" PRIu16 "\n", r.BaseArray());
		printf("\t\t LastArray()     = %" PRIu16 "\n", r.LastArray());
		printf("\t\t MinLodWarn()    = %" PRIu16 "\n", r.MinLodWarn());
		printf("\t\t CounterBankId() = %" PRIu8 "\n", r.CounterBankId());
		printf("\t\t LodHdwCntEn()   = %s\n", r.LodHdwCntEn() ? "true" : "false");
		printf("\t\t slot            = %d\n", bind.textures2D.desc[i].slot);
		printf("\t\t start_register  = %d\n", bind.textures2D.desc[i].start_register);
		printf("\t\t extended        = %s\n", (bind.textures2D.desc[i].extended ? "true" : "false"));
		printf("\t\t usage           = %s\n", Core::EnumName(bind.textures2D.desc[i].usage).C_Str());
	}

	for (int i = 0; i < bind.samplers.samplers_num; i++)
	{
		const auto& r = bind.samplers.samplers[i];

		printf("\t Sampler %d\n", i);

		printf("\t\t fields = %08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "\n", r.fields[3], r.fields[2], r.fields[1], r.fields[0]);

		printf("\t\t ClampX()           = %" PRIu8 "\n", r.ClampX());
		printf("\t\t ClampY()           = %" PRIu8 "\n", r.ClampY());
		printf("\t\t ClampZ()           = %" PRIu8 "\n", r.ClampZ());
		printf("\t\t MaxAnisoRatio()    = %" PRIu8 "\n", r.MaxAnisoRatio());
		printf("\t\t DepthCompareFunc() = %" PRIu8 "\n", r.DepthCompareFunc());
		printf("\t\t ForceUnormCoords() = %s\n", r.ForceUnormCoords() ? "true" : "false");
		printf("\t\t AnisoThreshold()   = %" PRIu8 "\n", r.AnisoThreshold());
		printf("\t\t McCoordTrunc()     = %s\n", r.McCoordTrunc() ? "true" : "false");
		printf("\t\t ForceDegamma()     = %s\n", r.ForceDegamma() ? "true" : "false");
		printf("\t\t AnisoBias()        = %" PRIu8 "\n", r.AnisoBias());
		printf("\t\t TruncCoord()       = %s\n", r.TruncCoord() ? "true" : "false");
		printf("\t\t DisableCubeWrap()  = %s\n", r.DisableCubeWrap() ? "true" : "false");
		printf("\t\t FilterMode()       = %" PRIu8 "\n", r.FilterMode());
		printf("\t\t MinLod()           = %" PRIu16 "\n", r.MinLod());
		printf("\t\t MaxLod()           = %" PRIu16 "\n", r.MaxLod());
		printf("\t\t PerfMip()          = %" PRIu8 "\n", r.PerfMip());
		printf("\t\t PerfZ()            = %" PRIu8 "\n", r.PerfZ());
		printf("\t\t LodBias()          = %" PRIu16 "\n", r.LodBias());
		printf("\t\t LodBiasSec()       = %" PRIu8 "\n", r.LodBiasSec());
		printf("\t\t XyMagFilter()      = %" PRIu8 "\n", r.XyMagFilter());
		printf("\t\t XyMinFilter()      = %" PRIu8 "\n", r.XyMinFilter());
		printf("\t\t ZFilter()          = %" PRIu8 "\n", r.ZFilter());
		printf("\t\t MipFilter()        = %" PRIu8 "\n", r.MipFilter());
		printf("\t\t BorderColorPtr()   = %" PRIu16 "\n", r.BorderColorPtr());
		printf("\t\t BorderColorType()  = %" PRIu8 "\n", r.BorderColorType());
		printf("\t\t slot               = %d\n", bind.samplers.slots[i]);
		printf("\t\t start_register     = %d\n", bind.samplers.start_register[i]);
		printf("\t\t extended           = %s\n", (bind.samplers.extended[i] ? "true" : "false"));
	}

	for (int i = 0; i < bind.gds_pointers.pointers_num; i++)
	{
		const auto& r = bind.gds_pointers.pointers[i];

		printf("\t Gds Pointer %d\n", i);

		printf("\t\t field = %08" PRIx32 "\n", r.field);

		printf("\t\t Base()         = %" PRIu16 "\n", r.Base());
		printf("\t\t Size()         = %" PRIu16 "\n", r.Size());
		printf("\t\t slot           = %d\n", bind.gds_pointers.slots[i]);
		printf("\t\t start_register = %d\n", bind.gds_pointers.start_register[i]);
		printf("\t\t extended       = %s\n", (bind.gds_pointers.extended[i] ? "true" : "false"));
	}
}

void ShaderDbgDumpInputInfo(const ShaderVertexInputInfo* info)
{
	KYTY_PROFILER_BLOCK("ShaderDbgDumpInputInfo(Vs)");

	printf("ShaderDbgDumpInputInfo()\n");

	printf("\t fetch        = %s\n", info->fetch ? "true" : "false");
	printf("\t export_count = %d\n", info->export_count);

	for (int i = 0; i < info->resources_num; i++)
	{
		printf("\t input %d\n", i);

		const auto& r  = info->resources[i];
		const auto& rd = info->resources_dst[i];

		printf("\t\t register_start   = %d\n", rd.register_start);
		printf("\t\t registers_num    = %d\n", rd.registers_num);
		printf("\t\t fields           = %08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "\n", r.fields[3], r.fields[2], r.fields[1],
		       r.fields[0]);
		printf("\t\t Base()           = %" PRIx64 "\n", r.Base());
		printf("\t\t Stride()         = %" PRIu16 "\n", r.Stride());
		printf("\t\t SwizzleEnabled() = %s\n", r.SwizzleEnabled() ? "true" : "false");
		printf("\t\t NumRecords()     = %" PRIu32 "\n", r.NumRecords());
		printf("\t\t DstSelX()        = %" PRIu8 "\n", r.DstSelX());
		printf("\t\t DstSelY()        = %" PRIu8 "\n", r.DstSelY());
		printf("\t\t DstSelZ()        = %" PRIu8 "\n", r.DstSelZ());
		printf("\t\t DstSelW()        = %" PRIu8 "\n", r.DstSelW());
		printf("\t\t Nfmt()           = %" PRIu8 "\n", r.Nfmt());
		printf("\t\t Dfmt()           = %" PRIu8 "\n", r.Dfmt());
		printf("\t\t MemoryType()     = 0x%02" PRIx8 "\n", r.MemoryType());
		printf("\t\t AddTid()         = %s\n", r.AddTid() ? "true" : "false");
	}

	for (int i = 0; i < info->buffers_num; i++)
	{
		printf("\t buffer %d\n", i);

		const auto& r = info->buffers[i];
		printf("\t\t addr        = %" PRIx64 "\n", r.addr);
		printf("\t\t stride      = %" PRIu32 "\n", r.stride);
		printf("\t\t num_records = %" PRIu32 "\n", r.num_records);
		printf("\t\t attr_num    = %" PRId32 "\n", r.attr_num);
		for (int j = 0; j < r.attr_num; j++)
		{
			printf("\t\t attr_indices[%d]  = %d\n", j, r.attr_indices[j]);
			printf("\t\t attr_offsets[%d]  = %u\n", j, r.attr_offsets[j]);
		}
	}

	ShaderDbgDumpResources(info->bind);
}

void ShaderDbgDumpInputInfo(const ShaderPixelInputInfo* info)
{
	KYTY_PROFILER_BLOCK("ShaderDbgDumpInputInfo(Ps)");

	printf("ShaderDbgDumpInputInfo()\n");

	printf("\t input_num            = %u\n", info->input_num);
	printf("\t ps_pos_xy            = %s\n", info->ps_pos_xy ? "true" : "false");
	printf("\t ps_pixel_kill_enable = %s\n", info->ps_pixel_kill_enable ? "true" : "false");
	printf("\t ps_early_z           = %s\n", info->ps_early_z ? "true" : "false");
	printf("\t ps_execute_on_noop   = %s\n", info->ps_execute_on_noop ? "true" : "false");

	for (uint32_t i = 0; i < info->input_num; i++)
	{
		printf("\t interpolator_settings[%u] = %u\n", i, info->interpolator_settings[i]);
	}

	ShaderDbgDumpResources(info->bind);
}

void ShaderDbgDumpInputInfo(const ShaderComputeInputInfo* info)
{
	printf("ShaderDbgDumpInputInfo()\n");

	printf("\t workgroup_register = %d\n", info->workgroup_register);
	printf("\t thread_ids_num     = %d\n", info->thread_ids_num);
	printf("\t threads_num        = {%u, %u, %u}\n", info->threads_num[0], info->threads_num[1], info->threads_num[2]);
	printf("\t threadgroup_id     = {%s, %s, %s}\n", info->group_id[0] ? "true" : "false", info->group_id[1] ? "true" : "false",
	       info->group_id[2] ? "true" : "false");

	ShaderDbgDumpResources(info->bind);
}

class ShaderLogHelper
{
public:
	explicit ShaderLogHelper(const char* type)
	{
		static std::atomic_int id = 0;

		switch (Config::GetShaderLogDirection())
		{
			case Config::ShaderLogDirection::Console:
				m_console = true;
				m_enabled = true;
				break;
			case Config::ShaderLogDirection::File:
			{
				m_file_name = Config::GetShaderLogFolder().FixDirectorySlash() +
				              String::FromPrintf("%04d_%04d_shader_%s.log", GraphicsRunGetFrameNum(), id++, type);
				Core::File::CreateDirectories(m_file_name.DirectoryWithoutFilename());
				m_file.Create(m_file_name);
				if (m_file.IsInvalid())
				{
					printf(FG_BRIGHT_RED "Can't create file: %s\n" FG_DEFAULT, m_file_name.C_Str());
					m_enabled = false;
				}
				m_enabled = true;
				m_console = false;
			}
			break;
			default: m_enabled = false; break;
		}
	}
	virtual ~ShaderLogHelper()
	{
		if (m_enabled && !m_console && !m_file.IsInvalid())
		{
			m_file.Close();
		}
	}

	KYTY_CLASS_NO_COPY(ShaderLogHelper);

	void DumpOriginalShader(const ShaderCode& code)
	{
		if (m_enabled)
		{
			if (m_console)
			{
				printf("--------- Original Shader ---------\n");
				printf("crc32 = %08" PRIx32 "\n", code.GetCrc32());
				printf("hash0 = %08" PRIx32 "\n", code.GetHash0());
				printf("---------\n");
				printf("%s", code.DbgDump().C_Str());
				printf("---------\n");
			} else if (!m_file.IsInvalid())
			{
				m_file.Printf("--------- Original Shader ---------\n");
				m_file.Printf("crc32 = %08" PRIx32 "\n", code.GetCrc32());
				m_file.Printf("hash0 = %08" PRIx32 "\n", code.GetHash0());
				m_file.Printf("---------\n");
				m_file.Printf("%s", code.DbgDump().C_Str());
				m_file.Printf("---------\n");
			}
		}
	}

	void DumpRecompiledShader(const String& source)
	{
		if (m_enabled)
		{
			if (m_console)
			{
				printf("--------- Recompiled Shader ---------\n");
				printf("%s\n", source.C_Str());
				printf("---------\n");
			} else if (!m_file.IsInvalid())
			{
				m_file.Printf("--------- Recompiled Shader ---------\n");
				m_file.Printf("%s\n", source.C_Str());
				m_file.Printf("---------\n");
			}
		}
	}

	void DumpOptimizedShader(const Vector<uint32_t>& bin)
	{
		if (m_enabled)
		{
			String text;
			if (!SpirvDisassemble(bin.GetDataConst(), bin.Size(), &text))
			{
				EXIT("SpirvDisassemble() failed\n");
			}
			if (m_console)
			{
				printf("--------- Optimized Shader ---------\n");
				printf("%s\n", text.C_Str());
				printf("---------\n");
			} else if (!m_file.IsInvalid())
			{
				m_file.Printf("--------- Optimized Shader ---------\n");
				m_file.Printf("%s\n", Log::RemoveColors(text).C_Str());
				m_file.Printf("---------\n");
			}
		}
	}

	void DumpGlslShader(const Vector<uint32_t>& bin)
	{
		if (m_enabled)
		{
			String text;
			if (!SpirvToGlsl(bin.GetDataConst(), bin.Size(), &text))
			{
				EXIT("SpirvToGlsl() failed\n");
			}
			if (m_console)
			{
				printf("--------- Glsl Shader ---------\n");
				printf("%s\n", text.C_Str());
				printf("---------\n");
			} else if (!m_file.IsInvalid())
			{
				m_file.Printf("--------- Glsl Shader ---------\n");
				m_file.Printf("%s\n", Log::RemoveColors(text).C_Str());
				m_file.Printf("---------\n");
			}
		}
	}

	void DumpBinary(const Vector<uint32_t>& bin)
	{
		if (m_enabled && !m_console && !m_file.IsInvalid())
		{
			Core::File file;
			String     file_name = m_file_name.FilenameWithoutExtension() + U".spv";
			file.Create(file_name);
			if (file.IsInvalid())
			{
				printf(FG_BRIGHT_RED "Can't create file: %s\n" FG_DEFAULT, file_name.C_Str());
			} else
			{
				file.Write(bin.GetDataConst(), bin.Size() * 4);
				file.Close();
			}
		}
	}

private:
	bool       m_console = false;
	bool       m_enabled = false;
	Core::File m_file;
	String     m_file_name;
};

ShaderCode ShaderParseVS(const HW::VertexShaderInfo* regs)
{
	KYTY_PROFILER_FUNCTION(profiler::colors::Amber300);

	ShaderCode code;
	code.SetType(ShaderType::Vertex);

	if (regs->vs_embedded)
	{
		code.SetVsEmbedded(true);
		code.SetVsEmbeddedId(regs->vs_embedded_id);
	} else
	{
		const auto* src = reinterpret_cast<const uint32_t*>(regs->vs_regs.GetGpuAddress());

		EXIT_NOT_IMPLEMENTED(src == nullptr);

		vs_print("ShaderParseVS()", regs->vs_regs);
		vs_check(regs->vs_regs);

		const auto* header = GetBinaryInfo(src);

		EXIT_NOT_IMPLEMENTED(header == nullptr);

		bi_print("ShaderParseVS():ShaderBinaryInfo", *header);

		code.SetCrc32(header->crc32);
		code.SetHash0(header->hash0);
		shader_parse(0, src, nullptr, &code);

		if (g_debug_printfs != nullptr)
		{
			auto id = (static_cast<uint64_t>(header->hash0) << 32u) | header->crc32;
			if (auto index = g_debug_printfs->Find(id, [](auto cmd, auto id) { return cmd.id == id; }); g_debug_printfs->IndexValid(index))
			{
				code.GetDebugPrintfs() = g_debug_printfs->At(index).cmds;
			}
		}
	}

	return code;
}

Vector<uint32_t> ShaderRecompileVS(const ShaderCode& code, const ShaderVertexInputInfo* input_info)
{
	KYTY_PROFILER_FUNCTION(profiler::colors::Amber300);

	String           source;
	Vector<uint32_t> ret;
	ShaderLogHelper  log("vs");

	if (code.IsVsEmbedded())
	{
		source = SpirvGetEmbeddedVs(code.GetVsEmbeddedId());
	} else
	{
		for (int i = 0; i < input_info->bind.storage_buffers.buffers_num; i++)
		{
			const auto& r = input_info->bind.storage_buffers.buffers[i];
			EXIT_NOT_IMPLEMENTED(((r.Stride() * r.NumRecords()) & 0x3u) != 0);
		}

		log.DumpOriginalShader(code);

		source = SpirvGenerateSource(code, input_info, nullptr, nullptr);
	}

	log.DumpRecompiledShader(source);

	if (String err_msg; !SpirvRun(source, &ret, &err_msg))
	{
		EXIT("SpirvRun() failed:\n%s\n", err_msg.C_Str());
	}

	log.DumpOptimizedShader(ret);

	return ret;
}

ShaderCode ShaderParsePS(const HW::PixelShaderInfo* regs)
{
	KYTY_PROFILER_FUNCTION(profiler::colors::Blue300);

	ShaderCode code;
	code.SetType(ShaderType::Pixel);

	if (regs->ps_embedded)
	{
		code.SetPsEmbedded(true);
		code.SetPsEmbeddedId(regs->ps_embedded_id);
	} else
	{
		const auto* src = reinterpret_cast<const uint32_t*>(regs->ps_regs.data_addr);

		EXIT_NOT_IMPLEMENTED(src == nullptr);

		ps_print("ShaderParsePS()", regs->ps_regs);
		ps_check(regs->ps_regs);

		EXIT_NOT_IMPLEMENTED(regs->ps_regs.user_sgpr > regs->ps_user_sgpr.count);

		const auto* header = GetBinaryInfo(src);

		EXIT_NOT_IMPLEMENTED(header == nullptr);

		bi_print("ShaderParsePS():ShaderBinaryInfo", *header);

		code.SetCrc32(header->crc32);
		code.SetHash0(header->hash0);
		shader_parse(0, src, nullptr, &code);

		if (g_debug_printfs != nullptr)
		{
			auto id = (static_cast<uint64_t>(header->hash0) << 32u) | header->crc32;
			if (auto index = g_debug_printfs->Find(id, [](auto cmd, auto id) { return cmd.id == id; }); g_debug_printfs->IndexValid(index))
			{
				code.GetDebugPrintfs() = g_debug_printfs->At(index).cmds;
			}
		}
	}

	return code;
}

Vector<uint32_t> ShaderRecompilePS(const ShaderCode& code, const ShaderPixelInputInfo* input_info)
{
	KYTY_PROFILER_FUNCTION(profiler::colors::Blue300);

	String           source;
	Vector<uint32_t> ret;
	ShaderLogHelper  log("ps");

	if (code.IsPsEmbedded())
	{
		source = SpirvGetEmbeddedPs(code.GetPsEmbeddedId());
	} else
	{
		//		for (uint32_t i = 0; i < input_info->input_num; i++)
		//		{
		//			EXIT_NOT_IMPLEMENTED(input_info->interpolator_settings[i] != i);
		//		}

		for (int i = 0; i < input_info->bind.storage_buffers.buffers_num; i++)
		{
			const auto& r = input_info->bind.storage_buffers.buffers[i];
			EXIT_NOT_IMPLEMENTED(((r.Stride() * r.NumRecords()) & 0x3u) != 0);
		}

		log.DumpOriginalShader(code);

		source = SpirvGenerateSource(code, nullptr, input_info, nullptr);
	}

	log.DumpRecompiledShader(source);

	if (String err_msg; !SpirvRun(source, &ret, &err_msg))
	{
		EXIT("SpirvRun() failed:\n%s\n", err_msg.C_Str());
	}

	log.DumpOptimizedShader(ret);

	return ret;
}

ShaderCode ShaderParseCS(const HW::ComputeShaderInfo* regs)
{
	KYTY_PROFILER_FUNCTION(profiler::colors::CyanA700);

	const auto* src = reinterpret_cast<const uint32_t*>(regs->cs_regs.data_addr);

	EXIT_NOT_IMPLEMENTED(src == nullptr);

	cs_print("ShaderParseCS()", regs->cs_regs);
	cs_check(regs->cs_regs);

	EXIT_NOT_IMPLEMENTED(regs->cs_regs.user_sgpr > regs->cs_user_sgpr.count);

	const auto* header = GetBinaryInfo(src);

	EXIT_NOT_IMPLEMENTED(header == nullptr);

	bi_print("ShaderParseCS():ShaderBinaryInfo", *header);

	ShaderCode code;
	code.SetType(ShaderType::Compute);

	code.SetCrc32(header->crc32);
	code.SetHash0(header->hash0);
	shader_parse(0, src, nullptr, &code);

	if (g_debug_printfs != nullptr)
	{
		auto id = (static_cast<uint64_t>(header->hash0) << 32u) | header->crc32;
		if (auto index = g_debug_printfs->Find(id, [](auto cmd, auto id) { return cmd.id == id; }); g_debug_printfs->IndexValid(index))
		{
			code.GetDebugPrintfs() = g_debug_printfs->At(index).cmds;
		}
	}

	return code;
}

Vector<uint32_t> ShaderRecompileCS(const ShaderCode& code, const ShaderComputeInputInfo* input_info)
{
	KYTY_PROFILER_FUNCTION(profiler::colors::CyanA700);

	ShaderLogHelper log("cs");

	for (int i = 0; i < input_info->bind.storage_buffers.buffers_num; i++)
	{
		const auto& r = input_info->bind.storage_buffers.buffers[i];
		EXIT_NOT_IMPLEMENTED(((r.Stride() * r.NumRecords()) & 0x3u) != 0);
	}

	Vector<uint32_t> ret;

	log.DumpOriginalShader(code);

	auto source = SpirvGenerateSource(code, nullptr, nullptr, input_info);

	log.DumpRecompiledShader(source);

	if (String err_msg; !SpirvRun(source, &ret, &err_msg))
	{
		EXIT("SpirvRun() failed:\n%s\n", err_msg.C_Str());
	}

	log.DumpOptimizedShader(ret);
	// log.DumpGlslShader(ret);
	log.DumpBinary(ret);

	return ret;
}

//// NOLINTNEXTLINE(readability-function-cognitive-complexity)
// static ShaderBindParameters ShaderUpdateBindInfo(const ShaderCode& code, const ShaderBindResources* bind)
//{
//	ShaderBindParameters p {};
//
//	auto find_image_op = [&](int index, int s, bool& found, bool& without_sampler)
//	{
//		const auto& insts = code.GetInstructions();
//		int         size  = static_cast<int>(insts.Size());
//		for (int i = index; i < size; i++)
//		{
//			const auto& inst = insts.At(i);
//
//			if ((inst.dst.type == ShaderOperandType::Sgpr && s >= inst.dst.register_id && s < inst.dst.register_id + inst.dst.size) ||
//			    (inst.dst2.type == ShaderOperandType::Sgpr && s >= inst.dst2.register_id && s < inst.dst2.register_id + inst.dst2.size) ||
//			    inst.type == ShaderInstructionType::SEndpgm)
//			{
//				break;
//			}
//
//			if (inst.type == ShaderInstructionType::ImageStore || inst.type == ShaderInstructionType::ImageStoreMip)
//			{
//				if (inst.src[1].register_id == s)
//				{
//					EXIT_NOT_IMPLEMENTED(found && !without_sampler);
//					without_sampler = true;
//					found           = true;
//				}
//			} else if (inst.type == ShaderInstructionType::ImageSample || inst.type == ShaderInstructionType::ImageLoad)
//			{
//				if (inst.src[1].register_id == s)
//				{
//					EXIT_NOT_IMPLEMENTED(found && without_sampler);
//					without_sampler = false;
//					found           = true;
//				}
//			}
//		}
//	};
//
//	if (bind->textures2D.textures_num > 0)
//	{
//		const auto& insts = code.GetInstructions();
//
//		for (int ti = 0; ti < bind->textures2D.textures_num; ti++)
//		{
//			bool found = false;
//			if (bind->textures2D.desc[ti].extended)
//			{
//				int s = bind->extended.start_register;
//
//				int index = 0;
//				for (const auto& inst: insts)
//				{
//					if ((inst.dst.type == ShaderOperandType::Sgpr && s >= inst.dst.register_id &&
//					     s < inst.dst.register_id + inst.dst.size) ||
//					    (inst.dst2.type == ShaderOperandType::Sgpr && s >= inst.dst2.register_id &&
//					     s < inst.dst2.register_id + inst.dst2.size) ||
//					    inst.type == ShaderInstructionType::SEndpgm)
//					{
//						break;
//					}
//
//					if (inst.type == ShaderInstructionType::SLoadDwordx8 && inst.src[0].register_id == s &&
//					    static_cast<int>(inst.src[1].constant.u >> 2u) + 16 == bind->textures2D.desc[ti].start_register)
//					{
//						find_image_op(index + 1, inst.dst.register_id, found, p.textures2d_without_sampler[ti]);
//					}
//
//					index++;
//				}
//			} else
//			{
//				find_image_op(0, bind->textures2D.desc[ti].start_register, found, p.textures2d_without_sampler[ti]);
//			}
//
//			EXIT_NOT_IMPLEMENTED(!found);
//
//			if (p.textures2d_without_sampler[ti])
//			{
//				p.textures2d_storage_num++;
//			} else
//			{
//				p.textures2d_sampled_num++;
//			}
//		}
//	}
//	return p;
//}
//
// ShaderBindParameters ShaderGetBindParametersVS(const ShaderCode& code, const ShaderVertexInputInfo* input_info)
//{
//	return ShaderUpdateBindInfo(code, &input_info->bind);
//}
//
// ShaderBindParameters ShaderGetBindParametersPS(const ShaderCode& code, const ShaderPixelInputInfo* input_info)
//{
//	return ShaderUpdateBindInfo(code, &input_info->bind);
//}
//
// ShaderBindParameters ShaderGetBindParametersCS(const ShaderCode& code, const ShaderComputeInputInfo* input_info)
//{
//	return ShaderUpdateBindInfo(code, &input_info->bind);
//}

static void ShaderGetBindIds(ShaderId* ret, const ShaderBindResources& bind)
{
	ret->ids.Add(bind.storage_buffers.buffers_num);

	for (int i = 0; i < bind.storage_buffers.buffers_num; i++)
	{
		// const auto& r = bind.storage_buffers.buffers[i];

		// ret->ids.Add(static_cast<uint32_t>(r.SwizzleEnabled()));
		// ret->ids.Add(r.DstSelX());
		// ret->ids.Add(r.DstSelY());
		// ret->ids.Add(r.DstSelZ());
		// ret->ids.Add(r.DstSelW());
		// ret->ids.Add(r.Nfmt());
		// ret->ids.Add(r.Dfmt());
		// ret->ids.Add(static_cast<uint32_t>(r.AddTid()));
		ret->ids.Add(bind.storage_buffers.slots[i]);
		ret->ids.Add(bind.storage_buffers.start_register[i]);
		ret->ids.Add(static_cast<uint32_t>(bind.storage_buffers.extended[i]));
		ret->ids.Add(static_cast<uint32_t>(bind.storage_buffers.usages[i]));
	}

	ret->ids.Add(bind.textures2D.textures_num);

	for (int i = 0; i < bind.textures2D.textures_num; i++)
	{
		// const auto& r = bind.textures2D.textures[i];
		// ret->ids.Add(r.MinLod());
		// ret->ids.Add(r.Dfmt());
		// ret->ids.Add(r.Nfmt());
		// ret->ids.Add(r.Width());
		// ret->ids.Add(r.Height());
		// ret->ids.Add(r.PerfMod());
		// ret->ids.Add(static_cast<uint32_t>(r.Interlaced()));
		// ret->ids.Add(r.DstSelX());
		// ret->ids.Add(r.DstSelY());
		// ret->ids.Add(r.DstSelZ());
		// ret->ids.Add(r.DstSelW());
		// ret->ids.Add(r.BaseLevel());
		// ret->ids.Add(r.LastLevel());
		// ret->ids.Add(r.TilingIdx());
		// ret->ids.Add(static_cast<uint32_t>(r.Pow2Pad()));
		// ret->ids.Add(r.Type());
		// ret->ids.Add(r.Depth());
		// ret->ids.Add(r.Pitch());
		// ret->ids.Add(r.BaseArray());
		// ret->ids.Add(r.LastArray());
		// ret->ids.Add(r.MinLodWarn());
		// ret->ids.Add(r.CounterBankId());
		// ret->ids.Add(static_cast<uint32_t>(r.LodHdwCntEn()));
		ret->ids.Add(bind.textures2D.desc[i].slot);
		ret->ids.Add(bind.textures2D.desc[i].start_register);
		ret->ids.Add(static_cast<uint32_t>(bind.textures2D.desc[i].extended));
		ret->ids.Add(static_cast<uint32_t>(bind.textures2D.desc[i].usage));
	}

	ret->ids.Add(bind.samplers.samplers_num);

	for (int i = 0; i < bind.samplers.samplers_num; i++)
	{
		// const auto& r = bind.samplers.samplers[i];

		// ret->ids.Add(r.ClampX());
		// ret->ids.Add(r.ClampY());
		// ret->ids.Add(r.ClampZ());
		// ret->ids.Add(r.MaxAnisoRatio());
		// ret->ids.Add(r.DepthCompareFunc());
		// ret->ids.Add(static_cast<uint32_t>(r.ForceUnormCoords()));
		// ret->ids.Add(r.AnisoThreshold());
		// ret->ids.Add(static_cast<uint32_t>(r.McCoordTrunc()));
		// ret->ids.Add(static_cast<uint32_t>(r.ForceDegamma()));
		// ret->ids.Add(r.AnisoBias());
		// ret->ids.Add(static_cast<uint32_t>(r.TruncCoord()));
		// ret->ids.Add(static_cast<uint32_t>(r.DisableCubeWrap()));
		// ret->ids.Add(r.FilterMode());
		// ret->ids.Add(r.MinLod());
		// ret->ids.Add(r.MaxLod());
		// ret->ids.Add(r.PerfMip());
		// ret->ids.Add(r.PerfZ());
		// ret->ids.Add(r.LodBias());
		// ret->ids.Add(r.LodBiasSec());
		// ret->ids.Add(r.XyMagFilter());
		// ret->ids.Add(r.XyMinFilter());
		// ret->ids.Add(r.ZFilter());
		// ret->ids.Add(r.MipFilter());
		// ret->ids.Add(r.BorderColorPtr());
		// ret->ids.Add(r.BorderColorType());
		ret->ids.Add(bind.samplers.slots[i]);
		ret->ids.Add(bind.samplers.start_register[i]);
		ret->ids.Add(static_cast<uint32_t>(bind.samplers.extended[i]));
	}

	ret->ids.Add(bind.gds_pointers.pointers_num);

	for (int i = 0; i < bind.gds_pointers.pointers_num; i++)
	{
		// const auto& r = bind.gds_pointers.pointers[i];

		ret->ids.Add(bind.gds_pointers.slots[i]);
		ret->ids.Add(bind.gds_pointers.start_register[i]);
		ret->ids.Add(static_cast<uint32_t>(bind.gds_pointers.extended[i]));
	}

	ret->ids.Add(static_cast<uint32_t>(bind.extended.used));
	ret->ids.Add(bind.extended.slot);
	ret->ids.Add(bind.extended.start_register);
}

ShaderId ShaderGetIdVS(const HW::VertexShaderInfo* regs, const ShaderVertexInputInfo* input_info)
{
	KYTY_PROFILER_FUNCTION();

	ShaderId ret;

	if (regs->vs_embedded)
	{
		ret.ids.Add(regs->vs_embedded_id);
		return ret;
	}

	const auto* src = reinterpret_cast<const uint32_t*>(regs->vs_regs.GetGpuAddress());

	EXIT_NOT_IMPLEMENTED(src == nullptr);

	const auto* header = GetBinaryInfo(src);

	EXIT_NOT_IMPLEMENTED(header == nullptr);

	ret.ids.Expand(64);

	ret.hash0 = header->hash0;
	ret.crc32 = header->crc32;

	ret.ids.Add(header->length);
	ret.ids.Add(static_cast<uint32_t>(input_info->fetch));
	ret.ids.Add(input_info->resources_num);
	ret.ids.Add(input_info->export_count);

	for (int i = 0; i < input_info->resources_num; i++)
	{
		const auto& r  = input_info->resources[i];
		const auto& rd = input_info->resources_dst[i];

		ret.ids.Add(rd.register_start);
		ret.ids.Add(rd.registers_num);
		ret.ids.Add(r.Stride());
		ret.ids.Add(static_cast<uint32_t>(r.SwizzleEnabled()));
		ret.ids.Add(r.DstSelX());
		ret.ids.Add(r.DstSelY());
		ret.ids.Add(r.DstSelZ());
		ret.ids.Add(r.DstSelW());
		ret.ids.Add(r.Nfmt());
		ret.ids.Add(r.Dfmt());
		ret.ids.Add(static_cast<uint32_t>(r.AddTid()));
	}

	ret.ids.Add(input_info->buffers_num);

	for (int i = 0; i < input_info->buffers_num; i++)
	{
		const auto& r = input_info->buffers[i];
		ret.ids.Add(r.attr_num);
		ret.ids.Add(r.stride);
		for (int j = 0; j < r.attr_num; j++)
		{
			ret.ids.Add(r.attr_indices[j]);
			ret.ids.Add(r.attr_offsets[j]);
		}
	}

	ShaderGetBindIds(&ret, input_info->bind);

	return ret;
}

ShaderId ShaderGetIdPS(const HW::PixelShaderInfo* regs, const ShaderPixelInputInfo* input_info)
{
	KYTY_PROFILER_FUNCTION();

	ShaderId ret;

	if (regs->ps_embedded)
	{
		ret.ids.Add(regs->ps_embedded_id);
		return ret;
	}

	const auto* src = reinterpret_cast<const uint32_t*>(regs->ps_regs.data_addr);

	EXIT_NOT_IMPLEMENTED(src == nullptr);

	const auto* header = GetBinaryInfo(src);

	EXIT_NOT_IMPLEMENTED(header == nullptr);

	ret.ids.Expand(64);

	ret.hash0 = header->hash0;
	ret.crc32 = header->crc32;

	ret.ids.Add(header->length);
	ret.ids.Add(input_info->input_num);
	ret.ids.Add(static_cast<uint32_t>(input_info->ps_pos_xy));
	ret.ids.Add(static_cast<uint32_t>(input_info->ps_pixel_kill_enable));
	ret.ids.Add(static_cast<uint32_t>(input_info->ps_early_z));
	ret.ids.Add(static_cast<uint32_t>(input_info->ps_execute_on_noop));

	for (uint32_t i = 0; i < input_info->input_num; i++)
	{
		ret.ids.Add(input_info->interpolator_settings[i]);
	}

	ShaderGetBindIds(&ret, input_info->bind);

	return ret;
}

ShaderId ShaderGetIdCS(const HW::ComputeShaderInfo* regs, const ShaderComputeInputInfo* input_info)
{
	const auto* src = reinterpret_cast<const uint32_t*>(regs->cs_regs.data_addr);

	EXIT_NOT_IMPLEMENTED(src == nullptr);

	const auto* header = GetBinaryInfo(src);

	EXIT_NOT_IMPLEMENTED(header == nullptr);

	ShaderId ret;
	ret.ids.Expand(64);

	ret.hash0 = header->hash0;
	ret.crc32 = header->crc32;

	ret.ids.Add(header->length);

	ret.ids.Add(input_info->workgroup_register);
	ret.ids.Add(input_info->thread_ids_num);

	for (int i = 0; i < 3; i++)
	{
		ret.ids.Add(input_info->threads_num[i]);
		ret.ids.Add(static_cast<uint32_t>(input_info->group_id[i]));
	}

	ShaderGetBindIds(&ret, input_info->bind);

	return ret;
}

bool ShaderIsDisabled(uint64_t addr)
{
	const auto* src = reinterpret_cast<const uint32_t*>(addr);
	EXIT_NOT_IMPLEMENTED(src == nullptr);

	const auto* header = GetBinaryInfo(src);
	EXIT_NOT_IMPLEMENTED(header == nullptr);

	auto id = (static_cast<uint64_t>(header->hash0) << 32u) | header->crc32;

	bool disabled = (g_disabled_shaders != nullptr && g_disabled_shaders->Contains(id));

	printf("Shader 0x%016" PRIx64 ": id = 0x%016" PRIx64 " - %s\n", addr, id, (disabled ? "disabled" : "enabled"));

	return disabled;
}

void ShaderDisable(uint64_t id)
{
	if (g_disabled_shaders == nullptr)
	{
		g_disabled_shaders = new Vector<uint64_t>;
	}

	if (!g_disabled_shaders->Contains(id))
	{
		g_disabled_shaders->Add(id);
	}
}

void ShaderInjectDebugPrintf(uint64_t id, const ShaderDebugPrintf& cmd)
{
	if (g_debug_printfs == nullptr)
	{
		g_debug_printfs = new Vector<ShaderDebugPrintfCmds>;
	}

	for (auto& c: *g_debug_printfs)
	{
		if (c.id == id)
		{
			c.cmds.Add(cmd);
			return;
		}
	}

	ShaderDebugPrintfCmds c;
	c.id = id;
	c.cmds.Add(cmd);

	g_debug_printfs->Add(c);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
