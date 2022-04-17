#include "CodeGenLLVM.h"
#include "codegen/emitter/CodeGen.inline.h"
#include "codegen/emitter/data/Data.inline.h"

using namespace codegen;

#define __ _ir_builder.

// TODO: good module name?
// TODO: module name to constant?
CodeGenLLVM::CodeGenLLVM(const std::shared_ptr<semant::ClassNode> &root)
    : CodeGen(std::make_shared<KlassBuilderLLVM>(root)), _ir_builder(_context), _module("MainModule", _context),
      _runtime(_module), _data(_builder, _module, _runtime)
{
    DEBUG_ONLY(_table.set_printer([](const std::string &name, const Symbol &s) {
        LOG("Added symbol \"" + name + "\": " + static_cast<std::string>(s))
    }));
}

void CodeGenLLVM::add_fields()
{
    const auto &this_klass = _builder->klass(_current_class->_type->_string);
    for (auto field = this_klass->fields_begin(); field != this_klass->fields_end(); field++)
    {
        const auto &name = (*field)->_object->_object;
        _table.add_symbol(name, Symbol(this_klass->field_offset(field - this_klass->fields_begin())));
    }
}

void CodeGenLLVM::emit_class_method_inner(const std::shared_ptr<ast::Feature> &method)
{
    // it is dummies for basic classes. There are external symbols
    if (semant::Semant::is_basic_type(_current_class->_type))
    {
        return;
    }

    auto *const func = _module.getFunction(
        _builder->klass(_current_class->_type->_string)->method_full_name(method->_object->_object));

    GUARANTEE_DEBUG(func);

    // add formals to symbol table
    _table.push_scope();
    for (auto &arg : func->args())
    {
        _table.add_symbol(arg.getName(), &arg);
    }

    // Create a new basic block to start insertion into.
    auto *entry = llvm::BasicBlock::Create(_context, static_cast<std::string>(Names::ENTRY_BLOCK_NAME), func);
    __ SetInsertPoint(entry);

    __ CreateRet(emit_expr(method->_expr));

    // TODO: verifier don't compile
    // GUARANTEE_DEBUG(!llvm::verifyFunction(*func));

    _table.pop_scope();
}

void CodeGenLLVM::emit_class_init_method_inner()
{
    // String init method is runtime method
    if (semant::Semant::is_string(_current_class->_type))
    {
        return;
    }

    // Note, that init method don't init header
    const auto &current_class_name = _current_class->_type->_string;
    auto *const func = _module.getFunction(Names::init_method(current_class_name));

    GUARANTEE_DEBUG(func);

    // Create a new basic block to start insertion into.
    auto *entry = llvm::BasicBlock::Create(_context, static_cast<std::string>(Names::ENTRY_BLOCK_NAME), func);
    __ SetInsertPoint(entry);

    // call parent constructor
    if (!semant::Semant::is_empty_type(_current_class->_parent)) // Object moment
    {
        const auto &parent_init = Names::init_method(_current_class->_parent->_string);
        // TODO: maybe better way to pass args?
        __ CreateCall(_module.getFunction(parent_init), {func->getArg(0)}, Names::call(parent_init));
    }

    const auto &klass = _builder->klass(current_class_name);
    for (const auto &feature : _current_class->_features)
    {
        if (std::holds_alternative<ast::AttrFeature>(feature->_base))
        {
            llvm::Value *initial_val = nullptr;

            const auto &name = feature->_object->_object;

            const auto &this_field = _table.symbol(name);
            GUARANTEE_DEBUG(this_field._type == Symbol::FIELD); // impossible

            const auto &offset = this_field._value._offset;
            auto *const field_ptr =
                __ CreateStructGEP(_data.class_struct(klass), func->getArg(0), offset, Names::gep(name));

            if (feature->_expr)
            {
                initial_val = emit_expr(feature->_expr);
            }
            else if (semant::Semant::is_trivial_type(feature->_type))
            {
                initial_val = _data.init_value(feature->_type);
            }
            else if (!semant::Semant::is_native_type(feature->_type))
            {
                GUARANTEE_DEBUG(field_ptr->getType()->isPointerTy());
                initial_val = llvm::ConstantPointerNull::get(static_cast<llvm::PointerType *>(field_ptr->getType()));
            }
            else
            {
                GUARANTEE_DEBUG(semant::Semant::is_basic_type(_current_class->_type));
                if (semant::Semant::is_native_int(feature->_type))
                {
                    // TODO: maybe carry out type to class field?
                    // TODO: maybe carry out initial value to class field?
                    initial_val = llvm::ConstantInt::get(llvm::Type::getInt64Ty(_context), 0);
                }
                else if (semant::Semant::is_native_bool(feature->_type))
                {
                    // TODO: maybe carry out type to class field?
                    // TODO: maybe carry out initial value to class field?
                    // TODO: bool is 64 bit int now
                    initial_val = llvm::ConstantInt::get(llvm::Type::getInt64Ty(_context), 0);
                }
                else
                {
                    SHOULD_NOT_REACH_HERE();
                }
            }

            // TODO: check this inst twice
            __ CreateStore(initial_val, field_ptr);
        }
    }

    // TODO: is it correct?
    __ CreateRet(nullptr);

    // TODO: verifier don't compile
    // GUARANTEE_DEBUG(!llvm::verifyFunction(*func));
}

// TODO: all temporaries names to constants?
llvm::Value *CodeGenLLVM::emit_binary_expr_inner(const ast::BinaryExpression &expr)
{
    auto *const lhs = emit_expr(expr._lhs);
    auto *const rhs = emit_expr(expr._rhs);

    auto logical_result = false;
    llvm::Value *op_result = nullptr;

    if (!std::holds_alternative<ast::EqExpression>(expr._base))
    {
        auto *const lv = emit_load_int(lhs); // load real values
        auto *const rv = emit_load_int(rhs);

        op_result = std::visit(
            ast::overloaded{
                [&](const ast::MinusExpression &minus) -> llvm::Value * { return __ CreateSub(lv, rv, "sub_tmp"); },
                [&](const ast::PlusExpression &plus) -> llvm::Value * { return __ CreateAdd(lv, rv, "add_tmp"); },
                [&](const ast::DivExpression &div) -> llvm::Value * {
                    return __ CreateSDiv(lv, rv, "div_tmp"); /* TODO: SDiv? */
                },
                [&](const ast::MulExpression &mul) -> llvm::Value * { return __ CreateMul(lv, rv, "mul_tmp"); },
                [&](const ast::LTExpression &lt) -> llvm::Value * {
                    logical_result = true;
                    return __ CreateICmpSLT(lv, rv, "sl_tmp");
                },
                [&](const ast::LEExpression &le) -> llvm::Value * {
                    // TODO: check this twice!
                    logical_result = true;
                    return __ CreateICmpSLE(lv, rv, "le_tmp");
                },
                [&](const ast::EqExpression &le) -> llvm::Value * { return nullptr; }},
            expr._base);
    }
    else
    {
        logical_result = true;

        auto *const is_same_ref = __ CreateICmpEQ(lhs, rhs);

        // do control flow
        auto *const func = __ GetInsertBlock()->getParent();
        auto *const true_bb = llvm::BasicBlock::Create(_context, "eq_ref_check_true", func);
        auto *const false_bb = llvm::BasicBlock::Create(_context, "eq_ref_check_false");
        auto *const merge_bb = llvm::BasicBlock::Create(_context, "eq_ref_check_cont");

        __ CreateCondBr(is_same_ref, true_bb, false_bb);

        // true branch - just jump to merge
        __ SetInsertPoint(true_bb);
        __ CreateBr(merge_bb);

        // false branch - runtime call to equals
        func->getBasicBlockList().push_back(false_bb);
        __ SetInsertPoint(false_bb);

        GUARANTEE_DEBUG(_runtime.method(RuntimeMethodsNames[RuntimeMethods::EQUALS]));
        auto *equals_func = _runtime.method(RuntimeMethodsNames[RuntimeMethods::EQUALS])->_func;

        auto *const false_branch_res =
            __ CreateCall(equals_func, {lhs, rhs}, Names::call(RuntimeMethodsNames[RuntimeMethods::EQUALS]));
        __ CreateBr(merge_bb);

        // merge results
        func->getBasicBlockList().push_back(merge_bb);
        __ SetInsertPoint(merge_bb);
        auto *const res_type = equals_func->getReturnType();
        op_result = __ CreatePHI(res_type, 2, "eq_tmp");
        ((llvm::PHINode *)op_result)->addIncoming(llvm::ConstantInt::get(res_type, 1, true), true_bb);
        ((llvm::PHINode *)op_result)->addIncoming(false_branch_res, false_bb);
    }

    return logical_result ? emit_allocate_bool(op_result) : emit_allocate_int(op_result);
}

llvm::Value *CodeGenLLVM::emit_unary_expr_inner(const ast::UnaryExpression &expr)
{
}

llvm::Value *CodeGenLLVM::emit_bool_expr(const ast::BoolExpression &expr)
{
    return _data.bool_const(expr._value);
}

llvm::Value *CodeGenLLVM::emit_int_expr(const ast::IntExpression &expr)
{
    return _data.int_const(expr._value);
}

llvm::Value *CodeGenLLVM::emit_string_expr(const ast::StringExpression &expr)
{
    return _data.string_const(expr._string);
}

llvm::Value *CodeGenLLVM::emit_object_expr_inner(const ast::ObjectExpression &expr)
{
    const auto &object = _table.symbol(expr._object);
    if (object._type == Symbol::FIELD)
    {
        // object field
    }
    else
    {
        return object._value._ptr; // local object defined in let, case, formal parameter
    }
}

llvm::Value *CodeGenLLVM::emit_new_inner(const std::shared_ptr<ast::Type> &klass_type)
{
    if (!semant::Semant::is_self_type(klass_type))
    {
        const auto &klass = _builder->klass(klass_type->_string);
        auto *const klass_struct = _data.class_struct(klass);

        auto *const func = _runtime.method(RuntimeMethodsNames[RuntimeMethods::GC_ALLOC])->_func;

        // prepare tag, size and dispatch table
        auto *const tag = llvm::ConstantInt::get(_runtime.header_elem_type(HeaderLayout::Tag), klass->tag());
        auto *const size = llvm::ConstantInt::get(_runtime.header_elem_type(HeaderLayout::Size), klass->size());
        auto *const disp_table = _data.class_disp_tab(klass);
        auto *const raw_table =
            __ CreateBitCast(disp_table, _runtime.void_type()->getPointerTo(), Names::cast(disp_table->getName()));

        // call allocation and cast to this klass pointer
        auto *const raw_object =
            __ CreateCall(func, {tag, size, raw_table}, Names::call(RuntimeMethodsNames[RuntimeMethods::GC_ALLOC]));
        auto *const object =
            __ CreateBitCast(raw_object, klass_struct->getPointerTo(), Names::cast(klass_struct->getName()));

        // call init
        const auto init_method = Names::init_method(klass->name());
        __ CreateCall(_module.getFunction(init_method), {object}, Names::call(init_method));

        // object is ready
        return object;
    }
}

llvm::Value *CodeGenLLVM::emit_new_expr_inner(const ast::NewExpression &expr)
{
    return emit_new_inner(expr._type);
}

llvm::Value *CodeGenLLVM::emit_cases_expr_inner(const ast::CaseExpression &expr)
{
}

llvm::Value *CodeGenLLVM::emit_let_expr_inner(const ast::LetExpression &expr)
{
}

llvm::Value *CodeGenLLVM::emit_loop_expr_inner(const ast::WhileExpression &expr)
{
}

llvm::Value *CodeGenLLVM::emit_if_expr_inner(const ast::IfExpression &expr)
{
}

llvm::Value *CodeGenLLVM::emit_dispatch_expr_inner(const ast::DispatchExpression &expr)
{
    // prepare args
    std::vector<llvm::Value *> args;
    args.push_back(nullptr); // dummy for the first arg
    for (const auto &arg : expr._args)
    {
        args.push_back(emit_expr(arg));
    }

    // get receiver
    auto *const receiver = emit_expr(expr._expr);
    args[0] = receiver;

    const auto &method_name = expr._object->_object;
    auto *const call = std::visit(
        ast::overloaded{
            [&](const ast::VirtualDispatchExpression &disp) {
                const auto &klass =
                    _builder->klass(semant::Semant::exact_type(expr._expr->_type, _current_class->_type)->_string);
                const auto disp_table_name = Names::disp_table(klass->name());

                // get pointer on dispatch table pointer
                auto *const disp_table_ptr_ptr =
                    __ CreateStructGEP(receiver, HeaderLayout::DispatchTable, Names::gep(disp_table_name));

                // load dispatch table pointer
                auto *const disp_table_ptr = __ CreateLoad(disp_table_ptr_ptr, Names::load(disp_table_name));

                // get pointer on method address
                auto *const method_ptr =
                    __ CreateStructGEP(disp_table_ptr, klass->method_index(method_name), Names::gep(method_name));

                // load method address
                auto *const method = __ CreateLoad(method_ptr, Names::load(method_name));

                // call
                return __ CreateCall(method, args, Names::call(method_name));
            },
            [&](const ast::StaticDispatchExpression &disp) {
                auto *const method =
                    _module.getFunction(_builder->klass(disp._type->_string)->method_full_name(method_name));

                GUARANTEE_DEBUG(method);

                return __ CreateCall(method, args, Names::call(method_name));
            }},
        expr._base);

    return call;
}

llvm::Value *CodeGenLLVM::emit_assign_expr_inner(const ast::AssignExpression &expr)
{
}

llvm::Value *CodeGenLLVM::emit_load_int(const llvm::Value *int_obj)
{
}

llvm::Value *CodeGenLLVM::emit_allocate_int(const llvm::Value *val)
{
}

llvm::Value *CodeGenLLVM::emit_load_bool(const llvm::Value *bool_obj)
{
}

llvm::Value *CodeGenLLVM::emit_allocate_bool(const llvm::Value *val)
{
}

void CodeGenLLVM::emit_runtime_main()
{
    const auto &runtime_main =
        llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getInt32Ty(_context), {}, false),
                               llvm::Function::ExternalLinkage, static_cast<std::string>(RUNTIME_MAIN_FUNC), &_module);

    auto *entry = llvm::BasicBlock::Create(_context, static_cast<std::string>(Names::ENTRY_BLOCK_NAME), runtime_main);
    __ SetInsertPoint(entry);

    const auto main_klass = _builder->klass(MainClassName);
    auto *const main_object = emit_new_inner(main_klass->klass());

    const auto main_method = main_klass->method_full_name(MainMethodName);
    __ CreateCall(_module.getFunction(main_method), {main_object}, Names::call(main_method));

    __ CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(_context), 0));

    // TODO: verifier don't compile
    // GUARANTEE_DEBUG(!llvm::verifyFunction(*runtime_main));
}

void CodeGenLLVM::emit(const std::string &out_file)
{
    const std::string obj_file = out_file + static_cast<std::string>(EXT);

    _data.emit(obj_file);

    emit_class_code(_builder->root()); // emit
    emit_runtime_main();

    CODEGEN_VERBOSE_ONLY(_module.print(llvm::errs(), nullptr););

    const auto target_triple = llvm::sys::getDefaultTargetTriple();
    CODEGEN_VERBOSE_ONLY(LOG("Target arch: " + target_triple));

    // TODO: what we really need?
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();

    CODEGEN_VERBOSE_ONLY(LOG("Initialized llvm emitter."));

    std::string error;
    const auto *const target = llvm::TargetRegistry::lookupTarget(target_triple, error);
    // TODO: better way for error handling
    if (!target)
    {
        std::cerr << error << std::endl;
        exit(-1);
    }

    CODEGEN_VERBOSE_ONLY(LOG("Found target: " + std::string(target->getName())));

    // TODO: opportunity to select options
    // TODO: can be null?
    auto *const target_machine = target->createTargetMachine(target_triple, "generic", "", llvm::TargetOptions(),
                                                             llvm::Optional<llvm::Reloc::Model>());

    // TODO: any settings?
    _module.setDataLayout(target_machine->createDataLayout());
    _module.setTargetTriple(target_triple);

    CODEGEN_VERBOSE_ONLY(LOG("Initialized target machine."));

    std::error_code ec;
    llvm::raw_fd_ostream dest(obj_file, ec);
    // TODO: better way for error handling
    if (ec)
    {
        std::cerr << "Could not open file: " << ec.message() << std::endl;
        exit(-1);
    }

    // TODO: legacy is bad?
    llvm::legacy::PassManager pass;
    // TODO: better way for error handling
    // TODO: opportunity to select options
    if (target_machine->addPassesToEmitFile(pass, dest, nullptr, llvm::CGFT_ObjectFile))
    {
        std::cerr << "TargetMachine can't emit a file of this type!" << std::endl;
        exit(-1);
    }

    CODEGEN_VERBOSE_ONLY(LOG("Run llvm emitter."));

    pass.run(_module);
    dest.flush();

    CODEGEN_VERBOSE_ONLY(LOG("Finished llvm emitter."));
}