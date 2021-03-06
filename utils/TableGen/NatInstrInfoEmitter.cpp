//===- NatInstrInfoEmitter.cpp - Generate a Instruction Set Desc. --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This tablegen backend is responsible for emitting a description of the target
// instruction set for the code generator.
//
//===----------------------------------------------------------------------===//

#include "CodeGenDAGPatterns.h"
#include "CodeGenSchedule.h"
#include "CodeGenTarget.h"
#include "SequenceToOffsetTable.h"
#include "TableGenBackends.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <vector>

using namespace llvm;

namespace {
    class NatInstrInfoEmitter {
        RecordKeeper &Records;
        CodeGenDAGPatterns CDP;
        const CodeGenSchedModels &SchedModels;

    public:
        NatInstrInfoEmitter(RecordKeeper &R):
                Records(R), CDP(R), SchedModels(CDP.getTargetInfo().getSchedModels()) {}

        // run - Output the instruction set description.
        void run(raw_ostream &OS);

    private:
        void emitEnums(raw_ostream &OS);

        typedef std::map<std::vector<std::string>, unsigned> OperandInfoMapTy;

        /// The keys of this map are maps which have OpName enum values as their keys
        /// and instruction operand indices as their values.  The values of this map
        /// are lists of instruction names.
        typedef std::map<std::map<unsigned, unsigned>,
        std::vector<std::string> > OpNameMapTy;
        typedef std::map<std::string, unsigned>::iterator StrUintMapIter;
        void emitRecord(const CodeGenInstruction &Inst, unsigned Num,
                        Record *InstrInfo,
                        std::map<std::vector<Record*>, unsigned> &EL,
                        const OperandInfoMapTy &OpInfo,
                        raw_ostream &OS);
        void emitOperandTypesEnum(raw_ostream &OS, const CodeGenTarget &Target);
        void initOperandMapData(
                const std::vector<const CodeGenInstruction *> &NumberedInstructions,
                const std::string &Namespace,
                std::map<std::string, unsigned> &Operands,
                OpNameMapTy &OperandMap);
        void emitOperandNameMappings(raw_ostream &OS, const CodeGenTarget &Target,
                                     const std::vector<const CodeGenInstruction*> &NumberedInstructions);

        // Operand information.
        void EmitOperandInfo(raw_ostream &OS, OperandInfoMapTy &OperandInfoIDs);
        std::vector<std::string> GetOperandInfo(const CodeGenInstruction &Inst);
    };
} // end anonymous namespace

/*
static void PrintDefList(const std::vector<Record*> &Uses,
                         unsigned Num, raw_ostream &OS) {
    OS << "static const uint16_t ImplicitList" << Num << "[] = { ";
    for (unsigned i = 0, e = Uses.size(); i != e; ++i)
        OS << getQualifiedName(Uses[i]) << ", ";
    OS << "0 };\n";
}
*/

//===----------------------------------------------------------------------===//
// Operand Info Emission.
//===----------------------------------------------------------------------===//

std::vector<std::string>
NatInstrInfoEmitter::GetOperandInfo(const CodeGenInstruction &Inst) {
    std::vector<std::string> Result;

    for (auto &Op : Inst.Operands) {
        // Handle aggregate operands and normal operands the same way by expanding
        // either case into a list of operands for this op.
        std::vector<CGIOperandList::OperandInfo> OperandList;

        // This might be a multiple operand thing.  Targets like X86 have
        // registers in their multi-operand operands.  It may also be an anonymous
        // operand, which has a single operand, but no declared class for the
        // operand.
        DagInit *MIOI = Op.MIOperandInfo;

        if (!MIOI || MIOI->getNumArgs() == 0) {
            // Single, anonymous, operand.
            OperandList.push_back(Op);
        } else {
            for (unsigned j = 0, e = Op.MINumOperands; j != e; ++j) {
                OperandList.push_back(Op);

                Record *OpR = cast<DefInit>(MIOI->getArg(j))->getDef();
                OperandList.back().Rec = OpR;
            }
        }

        for (unsigned j = 0, e = OperandList.size(); j != e; ++j) {
            Record *OpR = OperandList[j].Rec;
            std::string Res;

            if (OpR->isSubClassOf("RegisterOperand"))
                OpR = OpR->getValueAsDef("RegClass");
            if (OpR->isSubClassOf("RegisterClass"))
                Res += getQualifiedName(OpR) + "RegClassID, ";
            else if (OpR->isSubClassOf("PointerLikeRegClass"))
                Res += utostr(OpR->getValueAsInt("RegClassKind")) + ", ";
            else
                // -1 means the operand does not have a fixed register class.
                Res += "-1, ";

            // Fill in applicable flags.
            Res += "0";

            // Ptr value whose register class is resolved via callback.
            if (OpR->isSubClassOf("PointerLikeRegClass"))
                Res += "|(1<<MCOI::LookupPtrRegClass)";

            // Predicate operands.  Check to see if the original unexpanded operand
            // was of type PredicateOp.
            if (Op.Rec->isSubClassOf("PredicateOp"))
                Res += "|(1<<MCOI::Predicate)";

            // Optional def operands.  Check to see if the original unexpanded operand
            // was of type OptionalDefOperand.
            if (Op.Rec->isSubClassOf("OptionalDefOperand"))
                Res += "|(1<<MCOI::OptionalDef)";

            // Fill in operand type.
            Res += ", ";
            assert(!Op.OperandType.empty() && "Invalid operand type.");
            Res += Op.OperandType;

            // Fill in constraint info.
            Res += ", ";

            const CGIOperandList::ConstraintInfo &Constraint =
                    Op.Constraints[j];
            if (Constraint.isNone())
                Res += "0";
            else if (Constraint.isEarlyClobber())
                Res += "(1 << MCOI::EARLY_CLOBBER)";
            else {
                assert(Constraint.isTied());
                Res += "((" + utostr(Constraint.getTiedOperand()) +
                       " << 16) | (1 << MCOI::TIED_TO))";
            }

            Result.push_back(Res);
        }
    }

    return Result;
}

void NatInstrInfoEmitter::EmitOperandInfo(raw_ostream &OS,
                                          OperandInfoMapTy &OperandInfoIDs) {
    // ID #0 is for no operand info.
    unsigned OperandListNum = 0;
    OperandInfoIDs[std::vector<std::string>()] = ++OperandListNum;

    OS << "\n";
    const CodeGenTarget &Target = CDP.getTargetInfo();
    for (const CodeGenInstruction *Inst : Target.instructions()) {
        std::vector<std::string> OperandInfo = GetOperandInfo(*Inst);
        unsigned &N = OperandInfoIDs[OperandInfo];
        if (N != 0) continue;

        N = ++OperandListNum;
        OS << "static const MCOperandInfo OperandInfo" << N << "[] = { ";
        for (const std::string &Info : OperandInfo)
            OS << "{ " << Info << " }, ";
        OS << "};\n";
    }
}

/// Initialize data structures for generating operand name mappings.
///
/// \param Operands [out] A map used to generate the OpName enum with operand
///        names as its keys and operand enum values as its values.
/// \param OperandMap [out] A map for representing the operand name mappings for
///        each instructions.  This is used to generate the OperandMap table as
///        well as the getNamedOperandIdx() function.
void NatInstrInfoEmitter::initOperandMapData(
        const std::vector<const CodeGenInstruction *> &NumberedInstructions,
        const std::string &Namespace,
        std::map<std::string, unsigned> &Operands,
        OpNameMapTy &OperandMap) {

    unsigned NumOperands = 0;
    for (const CodeGenInstruction *Inst : NumberedInstructions) {
        if (!Inst->TheDef->getValueAsBit("UseNamedOperandTable"))
            continue;
        std::map<unsigned, unsigned> OpList;
        for (const auto &Info : Inst->Operands) {
            StrUintMapIter I = Operands.find(Info.Name);

            if (I == Operands.end()) {
                I = Operands.insert(Operands.begin(),
                                    std::pair<std::string, unsigned>(Info.Name, NumOperands++));
            }
            OpList[I->second] = Info.MIOperandNo;
        }
        OperandMap[OpList].push_back(Namespace + "::" + Inst->TheDef->getName());
    }
}

/// Generate a table and function for looking up the indices of operands by
/// name.
///
/// This code generates:
/// - An enum in the llvm::TargetNamespace::OpName namespace, with one entry
///   for each operand name.
/// - A 2-dimensional table called OperandMap for mapping OpName enum values to
///   operand indices.
/// - A function called getNamedOperandIdx(uint16_t Opcode, uint16_t NamedIdx)
///   for looking up the operand index for an instruction, given a value from
///   OpName enum
void NatInstrInfoEmitter::emitOperandNameMappings(raw_ostream &OS,
                                                  const CodeGenTarget &Target,
                                                  const std::vector<const CodeGenInstruction*> &NumberedInstructions) {

    const std::string &Namespace = Target.getInstNamespace();
    std::string OpNameNS = "OpName";
    // Map of operand names to their enumeration value.  This will be used to
    // generate the OpName enum.
    std::map<std::string, unsigned> Operands;
    OpNameMapTy OperandMap;

    initOperandMapData(NumberedInstructions, Namespace, Operands, OperandMap);

    OS << "enum {\n";
    for (const auto &Op : Operands)
        OS << "  " << Op.first << " = " << Op.second << ",\n";

    OS << "OPERAND_LAST";
    OS << "\n};\n";

    OS << "int16_t getNamedOperandIdx(uint16_t Opcode, uint16_t NamedIdx) {\n";
    if (!Operands.empty()) {
        OS << "  static const int16_t OperandMap [][" << Operands.size()
        << "] = {\n";
        for (const auto &Entry : OperandMap) {
            const std::map<unsigned, unsigned> &OpList = Entry.first;
            OS << "{";

            // Emit a row of the OperandMap table
            for (unsigned i = 0, e = Operands.size(); i != e; ++i)
                OS << (OpList.count(i) == 0 ? -1 : (int)OpList.find(i)->second) << ", ";

            OS << "},\n";
        }
        OS << "};\n";

        OS << "  switch(Opcode) {\n";
        unsigned TableIndex = 0;
        for (const auto &Entry : OperandMap) {
            for (const std::string &Name : Entry.second)
                OS << "  case " << Name << ":\n";

            OS << "    return OperandMap[" << TableIndex++ << "][NamedIdx];\n";
        }
        OS << "    default: return -1;\n";
        OS << "  }\n";
    } else {
        // There are no operands, so no need to emit anything
        OS << "  return -1;\n";
    }
    OS << "}\n";
}

/// Generate an enum for all the operand types for this target, under the
/// llvm::TargetNamespace::OpTypes namespace.
/// Operand types are all definitions derived of the Operand Target.td class.
void NatInstrInfoEmitter::emitOperandTypesEnum(raw_ostream &OS,
                                               const CodeGenTarget &Target) {

    const std::string &Namespace = Target.getInstNamespace();
    std::vector<Record *> Operands = Records.getAllDerivedDefinitions("Operand");

    OS << "enum OperandType {\n";

    unsigned EnumVal = 0;
    for (const Record *Op : Operands) {
        if (!Op->isAnonymous())
            OS << "  " << Op->getName() << " = " << EnumVal << ",\n";
        ++EnumVal;
    }

    OS << "  OPERAND_TYPE_LIST_END" << "\n};\n";
}

//===----------------------------------------------------------------------===//
// Main Output.
//===----------------------------------------------------------------------===//

// run - Emit the main instruction description records for the target...
void NatInstrInfoEmitter::run(raw_ostream &OS) {
    emitEnums(OS);

    CodeGenTarget &Target = CDP.getTargetInfo();
    const std::string &TargetName = Target.getName();
    Record *InstrInfo = Target.getInstructionSet();

    // Keep track of all of the def lists we have emitted already.
    std::map<std::vector<Record*>, unsigned> EmittedLists;
    unsigned ListNumber = 0;

    /*
    // Emit all of the instruction's implicit uses and defs.
    for (const CodeGenInstruction *II : Target.instructions()) {
        Record *Inst = II->TheDef;
        std::vector<Record*> Uses = Inst->getValueAsListOfDefs("Uses");
        if (!Uses.empty()) {
            unsigned &IL = EmittedLists[Uses];
            if (!IL) PrintDefList(Uses, IL = ++ListNumber, OS);
        }
        std::vector<Record*> Defs = Inst->getValueAsListOfDefs("Defs");
        if (!Defs.empty()) {
            unsigned &IL = EmittedLists[Defs];
            if (!IL) PrintDefList(Defs, IL = ++ListNumber, OS);
        }
    }
    */

    OperandInfoMapTy OperandInfoIDs;

    // Emit all of the operand info records.
    EmitOperandInfo(OS, OperandInfoIDs);

    // Emit all of the MCInstrDesc records in their ENUM ordering.
    //
    OS << "\nextern const MCInstrDesc " << TargetName << "Insts[] = {\n";
    const std::vector<const CodeGenInstruction*> &NumberedInstructions =
            Target.getInstructionsByEnumValue();

    SequenceToOffsetTable<std::string> InstrNames;
    unsigned Num = 0;
    for (const CodeGenInstruction *Inst : NumberedInstructions) {
        // Keep a list of the instruction names.
        InstrNames.add(Inst->TheDef->getName());
        // Emit the record into the table.
        emitRecord(*Inst, Num++, InstrInfo, EmittedLists, OperandInfoIDs, OS);
    }
    OS << "};\n\n";

    // Emit the array of instruction names.
    InstrNames.layout();
    OS << "extern const char " << TargetName << "InstrNameData[] = {\n";
    InstrNames.emit(OS, printChar);
    OS << "};\n\n";

    OS << "extern const unsigned " << TargetName <<"InstrNameIndices[] = {";
    Num = 0;
    for (const CodeGenInstruction *Inst : NumberedInstructions) {
        // Newline every eight entries.
        if (Num % 8 == 0)
            OS << "\n    ";
        OS << InstrNames.get(Inst->TheDef->getName()) << "U, ";
        ++Num;
    }

    OS << "\n};\n\n";

    // MCInstrInfo initialization routine.
    OS << "static inline void Init" << TargetName
    << "MCInstrInfo(MCInstrInfo *II) {\n";
    OS << "  II->InitMCInstrInfo(" << TargetName << "Insts, "
    << TargetName << "InstrNameIndices, " << TargetName << "InstrNameData, "
    << NumberedInstructions.size() << ");\n}\n\n";

    OS << "#endif // GET_INSTRINFO_MC_DESC\n\n";

    // Create a TargetInstrInfo subclass to hide the MC layer initialization.
    OS << "\n#ifdef GET_INSTRINFO_HEADER\n";
    OS << "#undef GET_INSTRINFO_HEADER\n";

    std::string ClassName = TargetName + "GenInstrInfo";
    OS << "struct " << ClassName << " : public TargetInstrInfo {\n"
    << "  explicit " << ClassName
    << "(int CFSetupOpcode = -1, int CFDestroyOpcode = -1);\n"
    << "  ~" << ClassName << "() override {}\n"
    << "};\n";

    OS << "#endif // GET_INSTRINFO_HEADER\n\n";

    OS << "\n#ifdef GET_INSTRINFO_CTOR_DTOR\n";
    OS << "#undef GET_INSTRINFO_CTOR_DTOR\n";

    OS << "extern const MCInstrDesc " << TargetName << "Insts[];\n";
    OS << "extern const unsigned " << TargetName << "InstrNameIndices[];\n";
    OS << "extern const char " << TargetName << "InstrNameData[];\n";
    OS << ClassName << "::" << ClassName
    << "(int CFSetupOpcode, int CFDestroyOpcode)\n"
    << "  : TargetInstrInfo(CFSetupOpcode, CFDestroyOpcode) {\n"
    << "  InitMCInstrInfo(" << TargetName << "Insts, " << TargetName
    << "InstrNameIndices, " << TargetName << "InstrNameData, "
    << NumberedInstructions.size() << ");\n}\n";

    emitOperandNameMappings(OS, Target, NumberedInstructions);

    emitOperandTypesEnum(OS, Target);
}

void NatInstrInfoEmitter::emitRecord(const CodeGenInstruction &Inst, unsigned Num,
                                     Record *InstrInfo,
                                     std::map<std::vector<Record*>, unsigned> &EmittedLists,
                                     const OperandInfoMapTy &OpInfo,
                                     raw_ostream &OS) {
    int MinOperands = 0;
    if (!Inst.Operands.empty())
        // Each logical operand can be multiple MI operands.
        MinOperands = Inst.Operands.back().MIOperandNo +
                      Inst.Operands.back().MINumOperands;

    /*
  unsigned short Opcode;        // The opcode number
  unsigned short NumOperands;   // Num of args (may be more if variable_ops)
  unsigned char NumDefs;        // Num of args that are definitions
  unsigned char Size;           // Number of bytes in encoding.
  unsigned short SchedClass;    // enum identifying instr sched class
  uint64_t Flags;               // Flags identifying machine instr class
  uint64_t TSFlags;             // Target Specific Flag values
  const uint16_t *ImplicitUses; // Registers implicitly read by this instr
  const uint16_t *ImplicitDefs; // Registers implicitly defined by this instr
  const MCOperandInfo *OpInfo;  // 'NumOperands' entries about operands
  // Subtarget feature that this is deprecated on, if any
  // -1 implies this is not deprecated by any single feature. It may still be
  // deprecated due to a "complex" reason, below.
  int64_t DeprecatedFeature;

  // A complex method to determine is a certain is deprecated or not, and return
  // the reason for deprecation.
  bool (*ComplexDeprecationInfo)(MCInst &, const MCSubtargetInfo &,
                                 std::string &);
     */

    OS << "  { ";
    OS << Num << ",\t" << MinOperands << ",\t"
    << Inst.Operands.NumDefs << ",\t"
    << Inst.TheDef->getValueAsInt("Size") << ",\t"
    << SchedModels.getSchedClassIdx(Inst) << ",\t0";

    // Emit all of the target independent flags...
    if (Inst.isPseudo)                    OS << ", PSEUDO";
    if (Inst.isReturn)                    OS << ", RETURN";
    if (Inst.isBranch)                    OS << ", BRANCH";
    if (Inst.isIndirectBranch)            OS << ", INDIRECT_BRANCH";
    if (Inst.isCompare)                   OS << ", COMPARE";
    if (Inst.isMoveImm)                   OS << ", MOVE_IMM";
    if (Inst.isBitcast)                   OS << ", BITCAST";
    if (Inst.isSelect)                    OS << ", SELECT";
    if (Inst.isBarrier)                   OS << ", BARRIER";
    if (Inst.hasDelaySlot)                OS << ", DELAY_SLOT";
    if (Inst.isCall)                      OS << ", CALL";
    if (Inst.canFoldAsLoad)               OS << ", FOLDABLE_AS_LOAD";
    if (Inst.mayLoad)                     OS << ", MAY_LOAD";
    if (Inst.mayStore)                    OS << ", MAY_STORE";
    if (Inst.isPredicable)                OS << ", PREDICABLE";
    if (Inst.isConvertibleToThreeAddress) OS << ", CONVERTIBLE_TO_3ADDR";
    if (Inst.isCommutable)                OS << ", COMMUTABLE";
    if (Inst.isTerminator)                OS << ", TERMINATOR";
    if (Inst.isReMaterializable)          OS << ", REMATERIALIZABLE";
    if (Inst.isNotDuplicable)             OS << ", NOT_DUPLICABLE";
    if (Inst.Operands.hasOptionalDef)     OS << ", HAS_OPTIONAL_DEF";
    if (Inst.usesCustomInserter)          OS << ", USES_CUSTOM_INSERTER";
    if (Inst.hasPostISelHook)             OS << ", HAS_POST_ISEL_HOOK";
    if (Inst.Operands.isVariadic)         OS << ", VARIADIC";
    if (Inst.hasSideEffects)              OS << ", UNMODELED_SIDE_EFFECTS";
    if (Inst.isAsCheapAsAMove)            OS << ", CHEAP_AS_A_MOVE";
    if (Inst.hasExtraSrcRegAllocReq)      OS << ", EXTRA_SRC_REG_ALLOC_REQ";
    if (Inst.hasExtraDefRegAllocReq)      OS << ", EXTRA_DEF_REG_ALLOC_REQ";
    if (Inst.isRegSequence)               OS << ", REG_SEQUENCE";
    if (Inst.isExtractSubreg)             OS << ", EXTRACT_SUBREG";
    if (Inst.isInsertSubreg)              OS << ", INSERT_SUBREG";
    if (Inst.isConvergent)                OS << ", CONVERGENT";

    // Emit all of the target-specific flags...
    BitsInit *TSF = Inst.TheDef->getValueAsBitsInit("TSFlags");
    if (!TSF)
        PrintFatalError("no TSFlags?");
    uint64_t Value = 0;
    for (unsigned i = 0, e = TSF->getNumBits(); i != e; ++i) {
        if (BitInit *Bit = dyn_cast<BitInit>(TSF->getBit(i)))
            Value |= uint64_t(Bit->getValue()) << i;
        else
            PrintFatalError("Invalid TSFlags bit in " + Inst.TheDef->getName());
    }
    OS << ", 0x";
    OS.write_hex(Value);
    OS << "ULL, ";

    // Emit the implicit uses and defs lists...
    // FIXME do what PrintDefList did here
    std::vector<Record*> UseList = Inst.TheDef->getValueAsListOfDefs("Uses");
    if (UseList.empty())
        OS << "nullptr, ";
    else
        OS << "ImplicitList" << EmittedLists[UseList] << ", ";

    std::vector<Record*> DefList = Inst.TheDef->getValueAsListOfDefs("Defs");
    if (DefList.empty())
        OS << "nullptr, ";
    else
        OS << "ImplicitList" << EmittedLists[DefList] << ", ";

    // Emit the operand info.
    std::vector<std::string> OperandInfo = GetOperandInfo(Inst);
    if (OperandInfo.empty())
        OS << "nullptr";
    else
        OS << "OperandInfo" << OpInfo.find(OperandInfo)->second;

    CodeGenTarget &Target = CDP.getTargetInfo();
    if (Inst.HasComplexDeprecationPredicate)
        // Emit a function pointer to the complex predicate method.
        OS << ", -1 "
        << ",&get" << Inst.DeprecatedReason << "DeprecationInfo";
    else if (!Inst.DeprecatedReason.empty())
        // Emit the Subtarget feature.
        OS << ", " << Target.getInstNamespace() << "::" << Inst.DeprecatedReason
        << " ,nullptr";
    else
        // Instruction isn't deprecated.
        OS << ", -1 ,nullptr";

    OS << " },  // Inst #" << Num << " = " << Inst.TheDef->getName() << "\n";
}

// emitEnums - Print out enum values for all of the instructions.
void NatInstrInfoEmitter::emitEnums(raw_ostream &OS) {

    OS << "  instr_enums:\n";

    CodeGenTarget Target(Records);

    // We must emit the PHI opcode first...
    std::string Namespace = Target.getInstNamespace();

    if (Namespace.empty())
        PrintFatalError("No instructions defined!");

    const std::vector<const CodeGenInstruction*> &NumberedInstructions =
            Target.getInstructionsByEnumValue();

    unsigned Num = 0;
    for (const CodeGenInstruction *Inst : NumberedInstructions)
        OS << "    " << Inst->TheDef->getName() << ": " << Num++ << "\n";
    OS << "    INSTRUCTION_LIST_END: " << NumberedInstructions.size() << "\n";

    OS << "  sched_enums: \n";
    for (const auto &Class : SchedModels.explicit_classes())
        OS << "    " << Class.Name << ": " << Num++ << "\n";
    OS << "    SCHED_LIST_END: " << SchedModels.numInstrSchedClasses() << "\n";
}

namespace llvm {

    void EmitNatInstrInfo(RecordKeeper &RK, raw_ostream &OS) {
        NatInstrInfoEmitter(RK).run(OS);
        EmitMapTable(RK, OS);
    }

} // end llvm namespace


