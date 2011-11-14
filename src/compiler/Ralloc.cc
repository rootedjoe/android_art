/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Dalvik.h"
#include "CompilerInternals.h"
#include "Dataflow.h"
#include "codegen/Ralloc.h"

STATIC bool setFp(CompilationUnit* cUnit, int index, bool isFP) {
    bool change = false;
    if (cUnit->regLocation[index].highWord) {
        return change;
    }
    if (isFP && !cUnit->regLocation[index].fp) {
        cUnit->regLocation[index].fp = true;
        cUnit->regLocation[index].defined = true;
        change = true;
    }
    return change;
}

STATIC bool setCore(CompilationUnit* cUnit, int index, bool isCore) {
    bool change = false;
    if (cUnit->regLocation[index].highWord) {
        return change;
    }
    if (isCore && !cUnit->regLocation[index].defined) {
        cUnit->regLocation[index].core = true;
        cUnit->regLocation[index].defined = true;
        change = true;
    }
    return change;
}

STATIC bool remapNames(CompilationUnit* cUnit, BasicBlock* bb)
{
    if (bb->blockType != kDalvikByteCode && bb->blockType != kEntryBlock &&
        bb->blockType != kExitBlock)
        return false;

    for (MIR* mir = bb->firstMIRInsn; mir; mir = mir->next) {
        SSARepresentation *ssaRep = mir->ssaRep;
        if (ssaRep) {
            for (int i = 0; i < ssaRep->numUses; i++) {
                ssaRep->uses[i] = cUnit->phiAliasMap[ssaRep->uses[i]];
            }
            for (int i = 0; i < ssaRep->numDefs; i++) {
                ssaRep->defs[i] = cUnit->phiAliasMap[ssaRep->defs[i]];
            }
        }
    }
    return false;
}

/*
 * Infer types and sizes.  We don't need to track change on sizes,
 * as it doesn't propagate.  We're guaranteed at least one pass through
 * the cfg.
 */
STATIC bool inferTypeAndSize(CompilationUnit* cUnit, BasicBlock* bb)
{
    MIR *mir;
    bool changed = false;   // Did anything change?

    if (bb->dataFlowInfo == NULL) return false;
    if (bb->blockType != kDalvikByteCode && bb->blockType != kEntryBlock)
        return false;

    for (mir = bb->firstMIRInsn; mir; mir = mir->next) {
        SSARepresentation *ssaRep = mir->ssaRep;
        if (ssaRep) {
            int attrs = oatDataFlowAttributes[mir->dalvikInsn.opcode];

            // Handle defs
            if (attrs & (DF_DA | DF_DA_WIDE)) {
                if (attrs & DF_CORE_A) {
                    changed |= setCore(cUnit, ssaRep->defs[0], true);
                }
                if (attrs & DF_DA_WIDE) {
                    cUnit->regLocation[ssaRep->defs[0]].wide = true;
                    cUnit->regLocation[ssaRep->defs[1]].highWord = true;
                    DCHECK_EQ(oatS2VReg(cUnit, ssaRep->defs[0])+1,
                              oatS2VReg(cUnit, ssaRep->defs[1]));
                }
            }

            // Handles uses
            int next = 0;
            if (attrs & (DF_UA | DF_UA_WIDE)) {
                if (attrs & DF_CORE_A) {
                    changed |= setCore(cUnit, ssaRep->uses[next], true);
                }
                if (attrs & DF_UA_WIDE) {
                    cUnit->regLocation[ssaRep->uses[next]].wide = true;
                    cUnit->regLocation[ssaRep->uses[next + 1]].highWord = true;
                    DCHECK_EQ(oatS2VReg(cUnit, ssaRep->uses[next])+1,
                              oatS2VReg(cUnit, ssaRep->uses[next + 1]));
                    next += 2;
                } else {
                    next++;
                }
            }
            if (attrs & (DF_UB | DF_UB_WIDE)) {
                if (attrs & DF_CORE_B) {
                    changed |= setCore(cUnit, ssaRep->uses[next], true);
                }
                if (attrs & DF_UB_WIDE) {
                    cUnit->regLocation[ssaRep->uses[next]].wide = true;
                    cUnit->regLocation[ssaRep->uses[next + 1]].highWord = true;
                    DCHECK_EQ(oatS2VReg(cUnit, ssaRep->uses[next])+1,
                              oatS2VReg(cUnit, ssaRep->uses[next + 1]));
                    next += 2;
                } else {
                    next++;
                }
            }
            if (attrs & (DF_UC | DF_UC_WIDE)) {
                if (attrs & DF_CORE_C) {
                    changed |= setCore(cUnit, ssaRep->uses[next], true);
                }
                if (attrs & DF_UC_WIDE) {
                    cUnit->regLocation[ssaRep->uses[next]].wide = true;
                    cUnit->regLocation[ssaRep->uses[next + 1]].highWord = true;
                    DCHECK_EQ(oatS2VReg(cUnit, ssaRep->uses[next])+1,
                              oatS2VReg(cUnit, ssaRep->uses[next + 1]));
                }
            }

           // Special-case handling for format 35c/3rc invokes
           Opcode opcode = mir->dalvikInsn.opcode;
           int flags = (opcode >= kNumPackedOpcodes) ? 0 :
                dexGetFlagsFromOpcode(opcode);
            if ((flags & kInstrInvoke) &&
                (attrs & (DF_FORMAT_35C | DF_FORMAT_3RC))) {
                DCHECK_EQ(next, 0);
                int target_idx = mir->dalvikInsn.vB;
                const char* shorty =
                    oatGetShortyFromTargetIdx(cUnit, target_idx);
                int numUses = mir->dalvikInsn.vA;
                // If this is a non-static invoke, skip implicit "this"
                if (((mir->dalvikInsn.opcode != OP_INVOKE_STATIC) &&
                     (mir->dalvikInsn.opcode != OP_INVOKE_STATIC_RANGE))) {
                   cUnit->regLocation[ssaRep->uses[next]].defined = true;
                   cUnit->regLocation[ssaRep->uses[next]].core = true;
                   next++;
                }
                uint32_t cpos = 1;
                if (strlen(shorty) > 1) {
                    for (int i = next; i < numUses;) {
                        DCHECK_LT(cpos, strlen(shorty));
                        switch(shorty[cpos++]) {
                            case 'D':
                                ssaRep->fpUse[i] = true;
                                ssaRep->fpUse[i+1] = true;
                                cUnit->regLocation[ssaRep->uses[i]].wide = true;
                                cUnit->regLocation[ssaRep->uses[i+1]].highWord
                                    = true;
                                DCHECK_EQ(oatS2VReg(cUnit, ssaRep->uses[i])+1,
                                          oatS2VReg(cUnit, ssaRep->uses[i+1]));
                                i++;
                                break;
                            case 'J':
                                cUnit->regLocation[ssaRep->uses[i]].wide = true;
                                cUnit->regLocation[ssaRep->uses[i+1]].highWord
                                    = true;
                                DCHECK_EQ(oatS2VReg(cUnit, ssaRep->uses[i])+1,
                                          oatS2VReg(cUnit, ssaRep->uses[i+1]));
                                changed |= setCore(cUnit, ssaRep->uses[i],true);
                                i++;
                               break;
                            case 'F':
                                ssaRep->fpUse[i] = true;
                                break;
                           default:
                                changed |= setCore(cUnit,ssaRep->uses[i], true);
                                break;
                        }
                        i++;
                    }
                }
            }

            for (int i=0; ssaRep->fpUse && i< ssaRep->numUses; i++) {
                if (ssaRep->fpUse[i])
                    changed |= setFp(cUnit, ssaRep->uses[i], true);
            }
            for (int i=0; ssaRep->fpDef && i< ssaRep->numDefs; i++) {
                if (ssaRep->fpDef[i])
                    changed |= setFp(cUnit, ssaRep->defs[i], true);
            }
            // Special-case handling for moves & Phi
            if (attrs & (DF_IS_MOVE | DF_NULL_TRANSFER_N)) {
                // If any of our inputs or outputs is defined, set all
                bool definedFP = false;
                bool definedCore = false;
                definedFP |= (cUnit->regLocation[ssaRep->defs[0]].defined &&
                              cUnit->regLocation[ssaRep->defs[0]].fp);
                definedCore |= (cUnit->regLocation[ssaRep->defs[0]].defined &&
                                cUnit->regLocation[ssaRep->defs[0]].core);
                for (int i = 0; i < ssaRep->numUses; i++) {
                    definedFP |= (cUnit->regLocation[ssaRep->uses[i]].defined &&
                                  cUnit->regLocation[ssaRep->uses[i]].fp);
                    definedCore |= (cUnit->regLocation[ssaRep->uses[i]].defined
                                  && cUnit->regLocation[ssaRep->uses[i]].core);
                }
                /*
                 * TODO: cleaner fix
                 * We don't normally expect to see a Dalvik register
                 * definition used both as a floating point and core
                 * value.  However, the instruction rewriting that occurs
                 * during verification can eliminate some type information,
                 * leaving us confused.  The real fix here is either to
                 * add explicit type information to Dalvik byte codes,
                 * or to recognize OP_THROW_VERIFICATION_ERROR as
                 * an unconditional branch and support dead code elimination.
                 * As a workaround we can detect this situation and
                 * disable register promotion (which is the only thing that
                 * relies on distinctions between core and fp usages.
                 */
                if ((definedFP && definedCore) &&
                    ((cUnit->disableOpt & (1 << kPromoteRegs)) == 0)) {
                    LOG(WARNING) << art::PrettyMethod(cUnit->method_idx, *cUnit->dex_file)
                        << " op at block " << bb->id
                        << " has both fp and core uses for same def.";
                    cUnit->disableOpt |= (1 << kPromoteRegs);
                }
                changed |= setFp(cUnit, ssaRep->defs[0], definedFP);
                changed |= setCore(cUnit, ssaRep->defs[0], definedCore);
                for (int i = 0; i < ssaRep->numUses; i++) {
                    changed |= setFp(cUnit, ssaRep->uses[i], definedFP);
                    changed |= setCore(cUnit, ssaRep->uses[i], definedCore);
                }
            }
        }
    }
    return changed;
}

static const char* storageName[] = {" Frame ", "PhysReg", " Spill "};

void oatDumpRegLocTable(RegLocation* table, int count)
{
    for (int i = 0; i < count; i++) {
        char buf[100];
        snprintf(buf, 100, "Loc[%02d] : %s, %c %c %c %c %c %c%d %c%d S%d",
             i, storageName[table[i].location], table[i].wide ? 'W' : 'N',
             table[i].defined ? 'D' : 'U', table[i].fp ? 'F' : 'C',
             table[i].highWord ? 'H' : 'L', table[i].home ? 'h' : 't',
             FPREG(table[i].lowReg) ? 's' : 'r', table[i].lowReg & FP_REG_MASK,
             FPREG(table[i].highReg) ? 's' : 'r', table[i].highReg & FP_REG_MASK,
             table[i].sRegLow);
        LOG(INFO) << buf;
    }
}

static const RegLocation freshLoc = {kLocDalvikFrame, 0, 0, 0, 0, 0, 0,
                                     INVALID_REG, INVALID_REG, INVALID_SREG};

/*
 * Simple register allocation.  Some Dalvik virtual registers may
 * be promoted to physical registers.  Most of the work for temp
 * allocation is done on the fly.  We also do some initilization and
 * type inference here.
 */
void oatSimpleRegAlloc(CompilationUnit* cUnit)
{
    int i;
    RegLocation* loc;

    /* Allocate the location map */
    loc = (RegLocation*)oatNew(cUnit->numSSARegs * sizeof(*loc), true);
    for (i=0; i< cUnit->numSSARegs; i++) {
        loc[i] = freshLoc;
        loc[i].sRegLow = i;
    }
    cUnit->regLocation = loc;

    /* Allocation the promotion map */
    int numRegs = cUnit->numDalvikRegisters;
    cUnit->promotionMap =
        (PromotionMap*)oatNew(numRegs * sizeof(cUnit->promotionMap[0]), true);

    /* Add types of incoming arguments based on signature */
    int numIns = cUnit->numIns;
    if (numIns > 0) {
        int sReg = numRegs - numIns;
        if ((cUnit->access_flags & art::kAccStatic) == 0) {
            // For non-static, skip past "this"
            cUnit->regLocation[sReg].defined = true;
            cUnit->regLocation[sReg].core = true;
            sReg++;
        }
        const char* shorty = cUnit->shorty;
        int shorty_len = strlen(shorty);
        for (int i = 1; i < shorty_len; i++) {
            switch(shorty[i]) {
                case 'D':
                    cUnit->regLocation[sReg].wide = true;
                    cUnit->regLocation[sReg+1].highWord = true;
                    DCHECK_EQ(oatS2VReg(cUnit, sReg)+1,
                              oatS2VReg(cUnit, sReg+1));
                    cUnit->regLocation[sReg].fp = true;
                    cUnit->regLocation[sReg].defined = true;
                    sReg++;
                    break;
                case 'J':
                    cUnit->regLocation[sReg].wide = true;
                    cUnit->regLocation[sReg+1].highWord = true;
                    DCHECK_EQ(oatS2VReg(cUnit, sReg)+1,
                              oatS2VReg(cUnit, sReg+1));
                    cUnit->regLocation[sReg].core = true;
                    cUnit->regLocation[sReg].defined = true;
                    sReg++;
                    break;
                case 'F':
                    cUnit->regLocation[sReg].fp = true;
                    cUnit->regLocation[sReg].defined = true;
                    break;
                default:
                    cUnit->regLocation[sReg].core = true;
                    cUnit->regLocation[sReg].defined = true;
                    break;
            }
            sReg++;
        }
    }

    /* Remap names */
    oatDataFlowAnalysisDispatcher(cUnit, remapNames,
                                  kPreOrderDFSTraversal,
                                  false /* isIterative */);

    /* Do type & size inference pass */
    oatDataFlowAnalysisDispatcher(cUnit, inferTypeAndSize,
                                  kPreOrderDFSTraversal,
                                  true /* isIterative */);

    /*
     * Set the sRegLow field to refer to the pre-SSA name of the
     * base Dalvik virtual register.  Once we add a better register
     * allocator, remove this remapping.
     */
    for (i=0; i < cUnit->numSSARegs; i++) {
        cUnit->regLocation[i].sRegLow =
                DECODE_REG(oatConvertSSARegToDalvik(cUnit, loc[i].sRegLow));
    }

    cUnit->coreSpillMask = 0;
    cUnit->fpSpillMask = 0;
    cUnit->numCoreSpills = 0;

    oatDoPromotion(cUnit);

    if (cUnit->printMe && !(cUnit->disableOpt & (1 << kPromoteRegs))) {
        LOG(INFO) << "After Promotion";
        oatDumpRegLocTable(cUnit->regLocation, cUnit->numSSARegs);
    }

    /* Figure out the frame size */
    cUnit->numPadding = (STACK_ALIGN_WORDS -
        (cUnit->numCoreSpills + cUnit->numFPSpills + cUnit->numRegs +
         cUnit->numOuts + 2)) & (STACK_ALIGN_WORDS-1);
    cUnit->frameSize = (cUnit->numCoreSpills + cUnit->numFPSpills +
                        cUnit->numRegs + cUnit->numOuts +
                        cUnit->numPadding + 2) * 4;
    cUnit->insOffset = cUnit->frameSize + 4;
    cUnit->regsOffset = (cUnit->numOuts + cUnit->numPadding + 1) * 4;
}
