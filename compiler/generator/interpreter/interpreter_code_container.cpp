/************************************************************************
 ************************************************************************
    FAUST compiler
    Copyright (C) 2003-2015 GRAME, Centre National de Creation Musicale
    ---------------------------------------------------------------------
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 ************************************************************************
 ************************************************************************/

#include "exception.hh"
#include "Text.hh"
#include "floats.hh"
#include "global.hh"
#include "interpreter_code_container.hh"
#include "interpreter_optimizer.hh"

using namespace std;

/*
Interpretor : 
 
 - multiple unneeded cast are eliminated in CastNumInst
 - 'faustpower' recoded as pow(x,y)

*/

template <class T> map <string, FIRInstruction::Opcode> InterpreterInstVisitor<T>::gMathLibTable;

CodeContainer* InterpreterCodeContainer::createScalarContainer(const string& name, int sub_container_type)
{
    return new InterpreterScalarCodeContainer(name, 0, 1, sub_container_type);
}

CodeContainer* InterpreterCodeContainer::createContainer(const string& name, int numInputs, int numOutputs)
{
    CodeContainer* container;

    if (gGlobal->gOpenCLSwitch) {
        throw faustexception("ERROR : OpenCL not supported for Interpreter\n");
    }
    if (gGlobal->gCUDASwitch) {
        throw faustexception("ERROR : CUDA not supported for Interpreter\n");
    }

    if (gGlobal->gOpenMPSwitch) {
        throw faustexception("ERROR : OpenMP not supported for Interpreter\n");
    } else if (gGlobal->gSchedulerSwitch) {
        throw faustexception("ERROR : Scheduler not supported for Interpreter\n");
    } else if (gGlobal->gVectorSwitch) {
        throw faustexception("ERROR : Vector not supported for Interpreter\n");
    } else {
        container = new InterpreterScalarCodeContainer(name, numInputs, numOutputs, kInt);
    }

    return container;
}

// Scalar
InterpreterScalarCodeContainer::InterpreterScalarCodeContainer(const string& name,  int numInputs, int numOutputs, int sub_container_type)
    :InterpreterCodeContainer(name, numInputs, numOutputs)
{
     fSubContainerType = sub_container_type;
}

InterpreterScalarCodeContainer::~InterpreterScalarCodeContainer()
{}

void InterpreterCodeContainer::produceInternal()
{
    
}

interpreter_dsp_factory* InterpreterCodeContainer::produceModuleFloat()
{
    cout << "InterpreterCodeContainer::produceModuleFloat() " << fNumInputs << " " << fNumOutputs << endl;
    
    /*
    for (int i = 0; i < (int)FIRInstruction::kDeclare; i++) {
        std::cout << gFIRInstructionTable[i] << std::endl;
    }
    */
    
    // Add "fSamplingFreq" variable at offset 0 in HEAP
    if (!fGeneratedSR) {
        fDeclarationInstructions->pushBackInst(InstBuilder::genDecStructVar("fSamplingFreq", InstBuilder::genBasicTyped(Typed::kInt)));
    }
    
    cout << "-----generateGlobalDeclarations-----" << endl;
    generateGlobalDeclarations(&fCodeProducer);

    cout << "-----generateDeclarations-----" << endl;
    generateDeclarations(&fCodeProducer);
    
    //generateAllocate(&fCodeProducer);
    //generateDestroy(&fCodeProducer);
    
    cout << "-----generateStaticInit-----" << endl;
    generateStaticInit(&fCodeProducer);
    
    cout << "-----generateInit-----" << endl;
    generateInit(&fCodeProducer);
    
    FIRBlockInstruction<float>* init_block = fCodeProducer.fCurrentBlock;
    fCodeProducer.fCurrentBlock = new FIRBlockInstruction<float>();
    
    cout << "-----generateUserInterface-----" << endl;
    generateUserInterface(&fCodeProducer);
    
    // Generates local variables declaration and setup
    cout << "-----generateComputeBlock-----" << endl;
    generateComputeBlock(&fCodeProducer);
    
    FIRBlockInstruction<float>* compute_control_block = fCodeProducer.fCurrentBlock;
    fCodeProducer.fCurrentBlock = new FIRBlockInstruction<float>();

    // Generates one single scalar loop
    cout << "-----generateScalarLoop-----" << endl;
    ForLoopInst* loop = fCurLoop->generateScalarLoop(fFullCount);
    loop->accept(&fCodeProducer);
    
    FIRBlockInstruction<float>* compute_dsp_block = fCodeProducer.fCurrentBlock;
    
    // generateCompute(0);
    
    //generateComputeFunctions(&fCodeProducer);
    
    
    // Add kHalt in blocks
    init_block->push(new FIRBasicInstruction<float>(FIRInstruction::kHalt));
    compute_control_block->push(new FIRBasicInstruction<float>(FIRInstruction::kHalt));
    compute_dsp_block->push(new FIRBasicInstruction<float>(FIRInstruction::kHalt));
    
    // Bytecode optimization
    
    printf("fComputeDSPBlock size = %d\n", compute_dsp_block->size());
    
    // 1) optimize indexed 'heap' load/store in normal load/store
    FIRInstructionLoadStoreOptimizer<float> opt1;
    init_block = FIRInstructionOptimizer<float>::optimize(init_block, opt1);
    compute_control_block = FIRInstructionOptimizer<float>::optimize(compute_control_block, opt1);
    compute_dsp_block = FIRInstructionOptimizer<float>::optimize(compute_dsp_block, opt1);
    
    printf("fComputeDSPBlock size = %d\n", compute_dsp_block->size());
    
    // 2) then pptimize simple 'heap' load/store in move
    FIRInstructionMoveOptimizer<float> opt2;
    init_block = FIRInstructionOptimizer<float>::optimize(init_block, opt2);
    compute_control_block = FIRInstructionOptimizer<float>::optimize(compute_control_block, opt2);
    compute_dsp_block = FIRInstructionOptimizer<float>::optimize(compute_dsp_block, opt2);
    
    printf("fComputeDSPBlock size = %d\n", compute_dsp_block->size());
    
    // 3) them optimize 'heap' and 'direct' math operations
    FIRInstructionMathOptimizer<float> opt3;
    init_block = FIRInstructionOptimizer<float>::optimize(init_block, opt3);
    compute_control_block = FIRInstructionOptimizer<float>::optimize(compute_control_block, opt3);
    compute_dsp_block = FIRInstructionOptimizer<float>::optimize(compute_dsp_block, opt3);
    
    printf("fComputeDSPBlock size = %d\n", compute_dsp_block->size());
    
    // TODO
    /*
    int int_index = 0;
    int real_index = 0;
    compute_dsp_block->stackMove(int_index, real_index);
    printf("fComputeDSPBlock int stack = %d real stack = %d\n", int_index, real_index);
    */
   
    return new interpreter_dsp_factory(fNumInputs, fNumOutputs,
                                        fCodeProducer.fRealHeapOffset,
                                        fCodeProducer.fIntHeapOffset,
                                        fCodeProducer.fSROffset,
                                        fCodeProducer.fUserInterfaceBlock, 
                                        init_block,
                                        compute_control_block,
                                        compute_dsp_block);
}

/*
interpreter_dsp_aux<double>* InterpreterCodeContainer::produceModuleDouble()
{
    cout << "InterpreterCodeContainer::produceModuleDouble()" << endl;
    //return new interpreter_dsp_aux<double>(fNumInputs, fNumOutputs, 0, 0, NULL, NULL, NULL, NULL);
    return NULL;
}

interpreter_dsp_aux<quad>* InterpreterCodeContainer::produceModuleQuad()
{
    cout << "InterpreterCodeContainer::produceModuleQuad()" << endl;
    //return new interpreter_dsp_aux<quad>(fNumInputs, fNumOutputs, 0, 0, NULL, NULL, NULL, NULL);
    return NULL;
}
*/

void InterpreterCodeContainer::produceClass()
{
    /*
    printf("InterpreterCodeContainer::produceClass\n");
    
    // Add "fSamplingFreq" variable at offset 0 in HEAP
    pushDeclare(InstBuilder::genDecStructVar("fSamplingFreq", InstBuilder::genBasicTyped(Typed::kInt)));
    
    generateGlobalDeclarations(&fCodeProducer);

    generateDeclarations(&fCodeProducer);
    
    //generateAllocate(&fCodeProducer);
    //generateDestroy(&fCodeProducer);
    
    generateStaticInit(&fCodeProducer);
    
    //generateInit(&fCodeProducer);
    
    generateUserInterface(&fCodeProducer);
    
    generateCompute(0);
    
    //generateComputeFunctions(&fCodeProducer);
    */
}

void InterpreterCodeContainer::produceInfoFunctions(int tabs, const string& classname, bool isvirtual)
{}

void InterpreterScalarCodeContainer::generateCompute(int n)
{
    // Generates local variables declaration and setup
    //generateComputeBlock(&fCodeProducer);

    // Generates one single scalar loop
    ForLoopInst* loop = fCurLoop->generateScalarLoop(fFullCount);
    loop->accept(&fCodeProducer);
}
