#include "CodeGenLLVM.h"
#include "codegen/emitter/CodeGen.inline.h"
#include "codegen/emitter/data/Data.inline.h"

using namespace codegen;

#define __ _ir_builder.

CodeGenLLVM::CodeGenLLVM(const std::shared_ptr<semant::ClassNode> &root)
    : CodeGen(std::make_shared<KlassBuilderLLVM>(root)), _ir_builder(_context),
      _module(root->_class->_file_name, _context), _runtime(_module), _data(_builder, _module, _runtime),
      _true_obj(_data.bool_const(true)), _false_obj(_data.bool_const(false)),
      _true_val(llvm::ConstantInt::get(_runtime.default_int(), TrueValue)),
      _false_val(llvm::ConstantInt::get(_runtime.default_int(), FalseValue)),
      _int0_64(llvm::ConstantInt::get(_runtime.int64_type(), 0, true)),
      _int0_32(llvm::ConstantInt::get(_runtime.int32_type(), 0, true))
{
    GUARANTEE_DEBUG(_true_obj);
    GUARANTEE_DEBUG(_false_obj);

    DEBUG_ONLY(_table.set_printer([](const std::string &name, const Symbol &s) {
        LOG("Added symbol \"" + name + "\": " + static_cast<std::string>(s))
    }));
}

void CodeGenLLVM::add_fields()
{
    const auto &this_klass = _builder->klass(_current_class->_type->_string);
    for (auto field = this_klass->fields_begin(); field != this_klass->fields_end(); field++)
    {
        _table.add_symbol((*field)->_object->_object,
                          Symbol(this_klass->field_offset(field - this_klass->fields_begin()),
                                 semant::Semant::exact_type((*field)->_type, _current_class->_type)));
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

    // Create a new basic block to start insertion into.
    auto *entry = llvm::BasicBlock::Create(_context, Names::comment(Names::Comment::ENTRY_BLOCK), func);
    __ SetInsertPoint(entry);

    // add formals to symbol table
    // formals are local variables, so create their copy in this method and initialize by formals
    _table.push_scope();
    const auto &formals = std::get<ast::MethodFeature>(method->_base)._formals;
    for (auto i = 0; i < func->arg_size(); i++)
    {
        auto *const arg = func->getArg(i);

        const auto &local = __ CreateAlloca(arg->getType(), nullptr, arg->getName());
        __ CreateStore(arg, local);
        _table.add_symbol(static_cast<std::string>(arg->getName()),
                          Symbol(local, i != 0 ? formals[i - 1]->_type : _current_class->_type));
    }

    __ CreateRet(emit_expr(method->_expr));

    // TODO: verifier don't compile
    // GUARANTEE_DEBUG(!llvm::verifyFunction(*func));

    _table.pop_scope();
}

void CodeGenLLVM::emit_class_init_method_inner()
{
    const auto &klass = _builder->klass(_current_class->_type->_string);

    // Note, that init method don't init header
    auto *const func = _module.getFunction(klass->init_method());

    GUARANTEE_DEBUG(func);

    // Create a new basic block to start insertion into.
    auto *entry = llvm::BasicBlock::Create(_context, Names::comment(Names::Comment::ENTRY_BLOCK), func);
    __ SetInsertPoint(entry);

    // self is visible
    _table.push_scope();

    auto *const self_formal = func->getArg(0);
    const auto &local = __ CreateAlloca(self_formal->getType(), nullptr, self_formal->getName());
    __ CreateStore(self_formal, local);
    _table.add_symbol(SelfObject, Symbol(local, _current_class->_type));

    // set default value before init for fields of this class
    for (const auto &feature : _current_class->_features)
    {
        if (std::holds_alternative<ast::AttrFeature>(feature->_base))
        {
            llvm::Value *initial_val = nullptr;

            const auto &this_field = _table.symbol(feature->_object->_object);
            GUARANTEE_DEBUG(this_field._type == Symbol::FIELD); // impossible

            auto *const field_ptr =
                __ CreateStructGEP(_data.class_struct(klass), func->getArg(0), this_field._value._offset);

            if (semant::Semant::is_trivial_type(this_field._value_type))
            {
                initial_val = _data.init_value(this_field._value_type);
            }
            else if (!semant::Semant::is_native_type(this_field._value_type))
            {
                GUARANTEE_DEBUG(field_ptr->getType()->isPointerTy());
                initial_val = llvm::ConstantPointerNull::get(static_cast<llvm::PointerType *>(field_ptr->getType()));
            }
            else
            {
                GUARANTEE_DEBUG(semant::Semant::is_basic_type(_current_class->_type));
                if (semant::Semant::is_native_int(this_field._value_type) ||
                    semant::Semant::is_native_bool(this_field._value_type))
                {
                    initial_val = _int0_64;
                }
                else if (semant::Semant::is_native_string(this_field._value_type))
                {
                    initial_val = _data.make_char_string("");
                }
                else
                {
                    SHOULD_NOT_REACH_HERE();
                }
            }

            __ CreateStore(initial_val, field_ptr);
        }
    }

    // call parent constructor
    if (!semant::Semant::is_empty_type(_current_class->_parent)) // Object moment
    {
        const auto parent_init = _builder->klass(_current_class->_parent->_string)->init_method();

        __ CreateCall(_module.getFunction(parent_init), func->getArg(0),
                      Names::name(Names::Comment::CALL, parent_init));
    }

    // Now initialize
    for (const auto &feature : _current_class->_features)
    {
        if (std::holds_alternative<ast::AttrFeature>(feature->_base))
        {
            if (feature->_expr)
            {
                const auto &this_field = _table.symbol(feature->_object->_object);
                GUARANTEE_DEBUG(this_field._type == Symbol::FIELD); // impossible

                auto *const field_ptr =
                    __ CreateStructGEP(_data.class_struct(klass), func->getArg(0), this_field._value._offset);

                __ CreateStore(emit_expr(feature->_expr), field_ptr);
            }
        }
    }

    __ CreateRet(nullptr);

    _table.pop_scope();

    // TODO: verifier don't compile
    // GUARANTEE_DEBUG(!llvm::verifyFunction(*func));
}

void CodeGenLLVM::make_control_flow(llvm::Value *pred, llvm::BasicBlock *&true_block, llvm::BasicBlock *&false_block,
                                    llvm::BasicBlock *&merge_block)
{
    auto *const func = __ GetInsertBlock()->getParent();

    true_block = llvm::BasicBlock::Create(_context, Names::comment(Names::Comment::TRUE_BRANCH), func);
    false_block = llvm::BasicBlock::Create(_context, Names::comment(Names::Comment::FALSE_BRANCH));
    merge_block = llvm::BasicBlock::Create(_context, Names::comment(Names::Comment::MERGE_BLOCK));

    __ CreateCondBr(pred, true_block, false_block);

    __ SetInsertPoint(true_block);
}

llvm::Value *CodeGenLLVM::emit_ternary_operator(llvm::Value *pred, llvm::Value *true_val, llvm::Value *false_val,
                                                llvm::Type *type)
{
    auto *const func = __ GetInsertBlock()->getParent();

    llvm::BasicBlock *true_block = nullptr, *false_block = nullptr, *merge_block = nullptr;
    make_control_flow(pred, true_block, false_block, merge_block);

    // true block
    __ CreateBr(merge_block);

    // false block
    func->getBasicBlockList().push_back(false_block);
    __ SetInsertPoint(false_block);
    __ CreateBr(merge_block);

    // merge block
    func->getBasicBlockList().push_back(merge_block);
    __ SetInsertPoint(merge_block);
    auto *const result = __ CreatePHI(type, 2, Names::comment(Names::Comment::PHI));

    result->addIncoming(true_val, true_block);
    result->addIncoming(false_val, false_block);

    return result;
}

llvm::Value *CodeGenLLVM::emit_binary_expr_inner(const ast::BinaryExpression &expr,
                                                 const std::shared_ptr<ast::Type> &expr_type)
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
                [&](const ast::MinusExpression &minus) {
                    return __ CreateSub(lv, rv, Names::comment(Names::Comment::SUB));
                },
                [&](const ast::PlusExpression &plus) {
                    return __ CreateAdd(lv, rv, Names::comment(Names::Comment::ADD));
                },
                [&](const ast::DivExpression &div) {
                    return __ CreateSDiv(lv, rv, Names::comment(Names::Comment::DIV)); /* TODO: SDiv? */
                },
                [&](const ast::MulExpression &mul) {
                    return __ CreateMul(lv, rv, Names::comment(Names::Comment::MUL));
                },
                [&](const ast::LTExpression &lt) {
                    logical_result = true;
                    return emit_ternary_operator(__ CreateICmpSLT(lv, rv, Names::comment(Names::Comment::CMP_SLT)),
                                                 _true_obj, _false_obj, _true_obj->getType());
                },
                [&](const ast::LEExpression &le) {
                    // TODO: check this twice!
                    logical_result = true;
                    return emit_ternary_operator(__ CreateICmpSLE(lv, rv, Names::comment(Names::Comment::CMP_SLE)),
                                                 _true_obj, _false_obj, _true_obj->getType());
                },
                [&](const ast::EqExpression &le) { return static_cast<llvm::Value *>(nullptr); }},
            expr._base);
    }
    else
    {
        logical_result = true;

        // cast to void pointers for compare
        auto *const raw_lhs = __ CreateBitCast(lhs, _runtime.void_type()->getPointerTo());
        auto *const raw_rhs = __ CreateBitCast(rhs, _runtime.void_type()->getPointerTo());

        auto *const is_same_ref = __ CreateICmpEQ(raw_lhs, raw_rhs, Names::comment(Names::Comment::CMP_EQ));

        // do control flow
        auto *const func = __ GetInsertBlock()->getParent();

        llvm::BasicBlock *true_block = nullptr, *false_block = nullptr, *merge_block = nullptr;
        make_control_flow(is_same_ref, true_block, false_block, merge_block);

        // true branch - just jump to merge
        __ SetInsertPoint(true_block);
        __ CreateBr(merge_block);

        // false branch - runtime call to equals
        func->getBasicBlockList().push_back(false_block);
        __ SetInsertPoint(false_block);

        const auto &equals_func_id = RuntimeLLVM::RuntimeLLVMSymbols::EQUALS;
        auto *equals_func = _runtime.symbol_by_id(equals_func_id)->_func;

        auto *const eq_call_res = __ CreateCall(
            equals_func, {lhs, rhs}, Names::name(Names::Comment::CALL, _runtime.symbol_name(equals_func_id)));

        auto *const false_branch_res = emit_ternary_operator(
            __ CreateICmpEQ(eq_call_res, llvm::ConstantInt::get(equals_func->getReturnType(), TrueValue, true)),
            _true_obj, _false_obj, _true_obj->getType());

        false_block = __ GetInsertBlock(); // emit_ternary_operator changed cfg
        __ CreateBr(merge_block);

        // merge results
        func->getBasicBlockList().push_back(merge_block);
        __ SetInsertPoint(merge_block);
        op_result = __ CreatePHI(_true_obj->getType(), 2, Names::comment(Names::Comment::PHI));
        static_cast<llvm::PHINode *>(op_result)->addIncoming(_true_obj, true_block);
        static_cast<llvm::PHINode *>(op_result)->addIncoming(false_branch_res, false_block);
    }

    return logical_result ? op_result : emit_allocate_int(op_result);
}

llvm::Value *CodeGenLLVM::emit_unary_expr_inner(const ast::UnaryExpression &expr,
                                                const std::shared_ptr<ast::Type> &expr_type)
{
    auto *const operand = emit_expr(expr._expr);

    return std::visit(
        ast::overloaded{
            [&](const ast::IsVoidExpression &isvoid) {
                return emit_ternary_operator(
                    __ CreateICmpEQ(
                        operand, llvm::ConstantPointerNull::get(static_cast<llvm::PointerType *>(operand->getType())),
                        Names::name(Names::Comment::CMP_EQ)),
                    _true_obj, _false_obj, _true_obj->getType());
            },
            [&](const ast::NotExpression &) {
                auto *const not_res_val =
                    __ CreateXor(emit_load_bool(operand), _true_val, Names::name(Names::Comment::XOR));

                return emit_ternary_operator(
                    __ CreateICmpEQ(not_res_val, _true_val, Names::name(Names::Comment::CMP_EQ)), _true_obj, _false_obj,
                    _true_obj->getType());
            },
            [&](const ast::NegExpression &neg) {
                return emit_allocate_int(__ CreateNeg(emit_load_int(operand), Names::name(Names::Comment::NEG)));
            }},
        expr._base);
}

llvm::Value *CodeGenLLVM::emit_bool_expr(const ast::BoolExpression &expr, const std::shared_ptr<ast::Type> &expr_type)
{
    return _data.bool_const(expr._value);
}

llvm::Value *CodeGenLLVM::emit_int_expr(const ast::IntExpression &expr, const std::shared_ptr<ast::Type> &expr_type)
{
    return _data.int_const(expr._value);
}

llvm::Value *CodeGenLLVM::emit_string_expr(const ast::StringExpression &expr,
                                           const std::shared_ptr<ast::Type> &expr_type)
{
    return _data.string_const(expr._string);
}

llvm::Value *CodeGenLLVM::emit_object_expr_inner(const ast::ObjectExpression &expr,
                                                 const std::shared_ptr<ast::Type> &expr_type)
{
    const auto &object = _table.symbol(expr._object);

    auto *ptr = static_cast<llvm::Value *>(nullptr);
    auto type = std::shared_ptr<ast::Type>(nullptr);

    if (object._type == Symbol::FIELD)
    {
        const auto &klass = _builder->klass(_current_class->_type->_string);
        const auto &index = object._value._offset;

        ptr = __ CreateStructGEP(_data.class_struct(klass), emit_load_self(), index);
        type =
            semant::Semant::exact_type(static_pointer_cast<KlassLLVM>(klass)->field_type(index), _current_class->_type);
    }
    else
    {
        ptr = object._value._ptr;
        type = object._value_type;
    }

    return __ CreateLoad(_data.class_struct(_builder->klass(type->_string))->getPointerTo(), ptr, expr._object);
}

llvm::Value *CodeGenLLVM::emit_load_self()
{
    const auto &self_val = _table.symbol(SelfObject);

    return __ CreateLoad(_data.class_struct(_builder->klass(self_val._value_type->_string))->getPointerTo(),
                         self_val._value._ptr, SelfObject);
}

llvm::Value *CodeGenLLVM::emit_new_inner(const std::shared_ptr<ast::Type> &klass_type)
{
    const auto &alloc_func_id = RuntimeLLVM::RuntimeLLVMSymbols::GC_ALLOC;
    auto *const func = _runtime.symbol_by_id(alloc_func_id)->_func;
    const auto alloc_func_name = _runtime.symbol_name(alloc_func_id);

    if (!semant::Semant::is_self_type(klass_type))
    {
        const auto &klass = _builder->klass(klass_type->_string);

        // prepare tag, size and dispatch table
        auto *const tag = llvm::ConstantInt::get(_runtime.header_elem_type(HeaderLayout::Tag), klass->tag());
        auto *const size = llvm::ConstantInt::get(_runtime.header_elem_type(HeaderLayout::Size), klass->size());
        auto *const disp_tab = _data.class_disp_tab(klass);

        // call allocation and cast to this klass pointer
        auto *const raw_object =
            __ CreateCall(func, {tag, size, disp_tab}, Names::name(Names::Comment::CALL, alloc_func_name));
        auto *const object = __ CreateBitCast(raw_object, _data.class_struct(klass)->getPointerTo());

        // call init
        const auto init_method = klass->init_method();
        __ CreateCall(_module.getFunction(init_method), {object}, Names::name(Names::Comment::CALL, init_method));

        // object is ready
        return object;
    }

    auto *const self_val = emit_load_self();
    const auto &klass = _builder->klass(_current_class->_type->_string);
    const auto &klass_struct = _data.class_struct(klass);

    // get info about this object
    auto *const tag = emit_load_tag(self_val, klass_struct);
    auto *const size = emit_load_size(self_val, klass_struct);
    auto *const disp_tab = emit_load_dispatch_table(self_val, klass);

    // allocate memory
    auto *const raw_object =
        __ CreateCall(func, {tag, size, disp_tab}, Names::name(Names::Comment::CALL, alloc_func_name));

    // lookup init method
    auto *const class_obj_tab =
        _module.getNamedGlobal(_runtime.symbol_name(RuntimeLLVM::RuntimeLLVMSymbols::CLASS_OBJ_TAB));

    const auto init_method_name = klass->init_method();
    auto *const init_method_ptr = __ CreateGEP(class_obj_tab->getValueType(), class_obj_tab, {_int0_64, tag});

    // load init method and call
    // init method has the same type as for Object class
    auto *const object_init = _module.getFunction(init_method_name);
    auto *const init_method = __ CreateLoad(object_init->getType(), init_method_ptr, init_method_name);

    // call this init method
    __ CreateCall(object_init->getFunctionType(), init_method, {raw_object},
                  Names::name(Names::Comment::CALL, init_method_name));

    return raw_object;
}

llvm::Value *CodeGenLLVM::emit_new_expr_inner(const ast::NewExpression &expr,
                                              const std::shared_ptr<ast::Type> &expr_type)
{
    return emit_new_inner(expr._type);
}

llvm::Value *CodeGenLLVM::emit_load_tag(llvm::Value *obj, llvm::Type *obj_type)
{
    auto *const tag_ptr = __ CreateStructGEP(obj_type, obj, HeaderLayout::Tag);

    return __ CreateLoad(_runtime.header_elem_type(HeaderLayout::Tag), tag_ptr,
                         Names::name(Names::Comment::OBJ_TAG, static_cast<std::string>(obj->getName())));
}

llvm::Value *CodeGenLLVM::emit_load_size(llvm::Value *obj, llvm::Type *obj_type)
{
    auto *const size_ptr = __ CreateStructGEP(obj_type, obj, HeaderLayout::Size);

    return __ CreateLoad(_runtime.header_elem_type(HeaderLayout::Size), size_ptr,
                         Names::name(Names::Comment::OBJ_SIZE, static_cast<std::string>(obj->getName())));
}

llvm::Value *CodeGenLLVM::emit_load_dispatch_table(llvm::Value *obj, const std::shared_ptr<Klass> &klass)
{
    auto *const dispatch_table_ptr_ptr =
        __ CreateStructGEP(obj->getType()->getPointerElementType(), obj, HeaderLayout::DispatchTable);

    return __ CreateLoad(_data.class_disp_tab(klass)->getType(), dispatch_table_ptr_ptr,
                         Names::name(Names::Comment::OBJ_DISP_TAB, static_cast<std::string>(obj->getName())));
}

llvm::Value *CodeGenLLVM::emit_cases_expr_inner(const ast::CaseExpression &expr,
                                                const std::shared_ptr<ast::Type> &expr_type)
{
    auto *const pred = emit_expr(expr._expr);

    // we want to generate code for the the most precise cases first, so sort cases by tag
    // TODO: can be SELF_TYPE here?
    auto cases = expr._cases;
    std::sort(cases.begin(), cases.end(), [&](const auto &case_a, const auto &case_b) {
        return _builder->tag(case_b->_type->_string) < _builder->tag(case_a->_type->_string);
    });

    // save results and blocks for phi
    std::vector<std::pair<llvm::BasicBlock *, llvm::Value *>> results;

    auto *const func = __ GetInsertBlock()->getParent();

    auto *const is_not_null = __ CreateIsNotNull(pred, Names::comment(Names::Comment::NOT_NULL));

    llvm::BasicBlock *true_block = nullptr, *false_block = nullptr, *merge_block = nullptr;
    make_control_flow(is_not_null, true_block, false_block, merge_block);

    auto *const tag =
        emit_load_tag(pred, _data.class_struct(_builder->klass(
                                semant::Semant::exact_type(expr._expr->_type, _current_class->_type)->_string)));

    auto *const res_ptr_type =
        _data.class_struct(_builder->klass(semant::Semant::exact_type(expr_type, _current_class->_type)->_string))
            ->getPointerTo();

    // no, it is not void
    // Last case is a special case: branch to abort
    for (auto i = 0; i < cases.size(); i++)
    {
        // TODO: can be SELF_TYPE here?
        const auto &klass = _builder->klass(cases[i]->_type->_string);

        auto *const tag_type = _runtime.header_elem_type(HeaderLayout::Tag);
        // TODO: signed?
        // if object tag lower than the lowest tag for this branch, jump to next case
        auto *const less = __ CreateICmpSLT(tag, llvm::ConstantInt::get(tag_type, klass->tag()),
                                            Names::comment(Names::Comment::CMP_SLT));

        // TODO: signed?
        // if object tag higher that the highest tag for this branch, jump to next case
        auto *const higher = __ CreateICmpSGT(tag, llvm::ConstantInt::get(tag_type, klass->child_max_tag()),
                                              Names::comment(Names::Comment::CMP_SGT));

        auto *const need_next = __ CreateOr(less, higher, Names::comment(Names::Comment::OR));

        auto *match_block = llvm::BasicBlock::Create(_context, Names::comment(Names::Comment::FALSE_BRANCH));
        auto *const next_case = llvm::BasicBlock::Create(_context, Names::comment(Names::Comment::TRUE_BRANCH));

        // TODO: set weight?
        __ CreateCondBr(need_next, next_case, match_block);

        func->getBasicBlockList().push_back(match_block);
        __ SetInsertPoint(match_block);

        // match branch
        auto *const result = emit_in_scope(cases[i]->_object, cases[i]->_type, cases[i]->_expr, pred);
        auto *const casted_res = __ CreateBitCast(result, res_ptr_type);
        match_block = __ GetInsertBlock();
        results.push_back({match_block, casted_res});

        __ CreateBr(merge_block);

        func->getBasicBlockList().push_back(next_case);
        __ SetInsertPoint(next_case);
    }

    auto *const null_result = llvm::ConstantPointerNull::get(res_ptr_type);

    // did not find suitable branch
    const auto &case_abort_id = RuntimeLLVM::RuntimeLLVMSymbols::CASE_ABORT;
    auto *const case_abort = _runtime.symbol_by_id(case_abort_id);
    __ CreateCall(case_abort->_func->getFunctionType(), case_abort->_func, {tag},
                  Names::name(Names::CALL, _runtime.symbol_name(case_abort_id)));

    // do it just for phi
    results.push_back({__ GetInsertBlock(), null_result});
    __ CreateBr(merge_block);

    // pred is null
    func->getBasicBlockList().push_back(false_block);
    __ SetInsertPoint(false_block);
    const auto &case_abort_2_id = RuntimeLLVM::RuntimeLLVMSymbols::CASE_ABORT_2;
    auto *const case_abort_2_func = _runtime.symbol_by_id(case_abort_2_id)->_func;
    const auto case_abort_2_name = _runtime.symbol_name(case_abort_2_id);
    __ CreateCall(case_abort_2_func,
                  {_data.string_const(_current_class->_file_name),
                   llvm::ConstantInt::get(_runtime.int32_type(), expr._expr->_line_number)},
                  Names::name(Names::Comment::CALL, case_abort_2_name));
    __ CreateBr(merge_block);
    results.push_back({false_block, null_result});

    // return value
    func->getBasicBlockList().push_back(merge_block);
    __ SetInsertPoint(merge_block);

    auto *const result = __ CreatePHI(res_ptr_type, results.size(), Names::comment(Names::Comment::PHI));
    for (const auto &res : results)
    {
        result->addIncoming(res.second, res.first);
    }

    return result;
}

llvm::Value *CodeGenLLVM::emit_let_expr_inner(const ast::LetExpression &expr,
                                              const std::shared_ptr<ast::Type> &expr_type)
{
    return emit_in_scope(expr._object, expr._type, expr._body_expr, expr._expr ? emit_expr(expr._expr) : nullptr);
}

llvm::Value *CodeGenLLVM::emit_loop_expr_inner(const ast::WhileExpression &expr,
                                               const std::shared_ptr<ast::Type> &expr_type)
{
    auto *const func = __ GetInsertBlock()->getParent();

    auto *loop_header = llvm::BasicBlock::Create(_context, Names::comment(Names::Comment::LOOP_HEADER), func);
    auto *loop_body = llvm::BasicBlock::Create(_context, Names::comment(Names::Comment::LOOP_BODY));
    auto *loop_tail = llvm::BasicBlock::Create(_context, Names::comment(Names::Comment::LOOP_TAIL));

    __ CreateBr(loop_header);

    __ SetInsertPoint(loop_header);
    __ CreateCondBr(
        __ CreateICmpEQ(emit_load_bool(emit_expr(expr._predicate)), _true_val, Names::comment(Names::Comment::CMP_EQ)),
        loop_body, loop_tail);
    auto *const new_loop_header = __ GetInsertBlock();

    func->getBasicBlockList().push_back(loop_body);
    __ SetInsertPoint(loop_body);
    emit_expr(expr._body_expr);
    __ CreateBr(loop_header);
    loop_body = __ GetInsertBlock();

    func->getBasicBlockList().push_back(loop_tail);
    __ SetInsertPoint(loop_tail);

    return llvm::ConstantPointerNull::get(_data.class_struct(_builder->klass(expr_type->_string))->getPointerTo());
}

llvm::Value *CodeGenLLVM::emit_if_expr_inner(const ast::IfExpression &expr, const std::shared_ptr<ast::Type> &expr_type)
{
    // do control flow
    auto *const func = __ GetInsertBlock()->getParent();

    auto *const phi_type =
        _data.class_struct(_builder->klass(semant::Semant::exact_type(expr_type, _current_class->_type)->_string))
            ->getPointerTo();

    auto *const pred =
        __ CreateICmpEQ(emit_load_bool(emit_expr(expr._predicate)), _true_val, Names::comment(Names::Comment::CMP_EQ));

    llvm::BasicBlock *true_block = nullptr, *false_block = nullptr, *merge_block = nullptr;
    make_control_flow(pred, true_block, false_block, merge_block);

    // true branch
    auto *const true_bb_val = __ CreateBitCast(emit_expr(expr._true_path_expr), phi_type);
    __ CreateBr(merge_block);
    true_block = __ GetInsertBlock(); // emit_expr can change cfg

    // false branch
    func->getBasicBlockList().push_back(false_block);
    __ SetInsertPoint(false_block);
    auto *const false_bb_val = __ CreateBitCast(emit_expr(expr._false_path_expr), phi_type);
    __ CreateBr(merge_block);
    false_block = __ GetInsertBlock(); // emit_expr can change cfg

    // merge block
    func->getBasicBlockList().push_back(merge_block);
    __ SetInsertPoint(merge_block);

    auto *const phi = __ CreatePHI(phi_type, 2, Names::comment(Names::Comment::PHI));

    phi->addIncoming(true_bb_val, true_block);
    phi->addIncoming(false_bb_val, false_block);

    return phi;
}

llvm::Value *CodeGenLLVM::emit_dispatch_expr_inner(const ast::DispatchExpression &expr,
                                                   const std::shared_ptr<ast::Type> &expr_type)
{
    auto *const func = __ GetInsertBlock()->getParent();

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

    auto *const phi_type =
        _data.class_struct(_builder->klass(semant::Semant::exact_type(expr_type, _current_class->_type)->_string))
            ->getPointerTo();

    // check if receiver is null
    auto *const is_not_null = __ CreateIsNotNull(receiver, Names::comment(Names::Comment::NOT_NULL));

    llvm::BasicBlock *true_block = nullptr, *false_block = nullptr, *merge_block = nullptr;
    make_control_flow(is_not_null, true_block, false_block, merge_block);

    const auto &method_name = expr._object->_object;
    auto *const call = std::visit(
        ast::overloaded{
            [&](const ast::VirtualDispatchExpression &disp) {
                const auto &klass =
                    _builder->klass(semant::Semant::exact_type(expr._expr->_type, _current_class->_type)->_string);

                auto *const dispatch_table_ptr = emit_load_dispatch_table(receiver, klass);

                // get pointer on method address
                // method has the same type as in this klass
                auto *const base_method = _module.getFunction(klass->method_full_name(method_name));
                auto *const method_ptr = __ CreateStructGEP(_data.class_disp_tab(klass)->getValueType(),
                                                            dispatch_table_ptr, klass->method_index(method_name));

                // load method
                auto *const method = __ CreateLoad(base_method->getType(), method_ptr, method_name);

                // call
                return __ CreateCall(base_method->getFunctionType(), method, args,
                                     Names::name(Names::Comment::CALL, method_name));
            },
            [&](const ast::StaticDispatchExpression &disp) {
                // TODO: can be SELF_TYPE here?
                auto *const method =
                    _module.getFunction(_builder->klass(disp._type->_string)->method_full_name(method_name));

                GUARANTEE_DEBUG(method);

                return __ CreateCall(method, args, Names::name(Names::Comment::CALL, method_name));
            }},
        expr._base);
    auto *const casted_call = __ CreateBitCast(call, phi_type);
    __ CreateBr(merge_block);

    // it is null
    func->getBasicBlockList().push_back(false_block);
    __ SetInsertPoint(false_block);
    const auto &dispatch_abort_id = RuntimeLLVM::RuntimeLLVMSymbols::DISPATCH_ABORT;
    auto *const dispatch_abort_func = _runtime.symbol_by_id(dispatch_abort_id)->_func;
    const auto dispatch_abort_name = _runtime.symbol_name(dispatch_abort_id);
    __ CreateCall(dispatch_abort_func,
                  {_data.string_const(_current_class->_file_name),
                   llvm::ConstantInt::get(_runtime.int32_type(), expr._expr->_line_number)},
                  Names::name(Names::Comment::CALL, dispatch_abort_name));
    __ CreateBr(merge_block);

    // merge
    func->getBasicBlockList().push_back(merge_block);
    __ SetInsertPoint(merge_block);

    auto *const phi = __ CreatePHI(phi_type, 2, Names::comment(Names::Comment::PHI));

    phi->addIncoming(casted_call, true_block);
    phi->addIncoming(llvm::ConstantPointerNull::get(phi_type), false_block);

    return phi;
}

llvm::Value *CodeGenLLVM::emit_assign_expr_inner(const ast::AssignExpression &expr,
                                                 const std::shared_ptr<ast::Type> &expr_type)
{
    auto *const value = emit_expr(expr._expr);

    const auto &symbol = _table.symbol(expr._object->_object);

    __ CreateStore(value, symbol._type == Symbol::FIELD
                              ? __ CreateStructGEP(_data.class_struct(_builder->klass(_current_class->_type->_string)),
                                                   emit_load_self(), symbol._value._offset)
                              : symbol._value._ptr);

    // TODO: is it correct?
    return value;
}

llvm::Value *CodeGenLLVM::emit_load_int(llvm::Value *int_obj)
{
    return emit_load_primitive(int_obj, _data.class_struct(_builder->klass(BaseClassesNames[BaseClasses::INT])));
}

llvm::Value *CodeGenLLVM::emit_load_primitive(llvm::Value *obj, llvm::Type *obj_type)
{
    const auto &value_ptr = __ CreateStructGEP(obj_type, obj, HeaderLayout::DispatchTable + 1);

    return __ CreateLoad(static_cast<llvm::StructType *>(obj_type)->getElementType(HeaderLayout::DispatchTable + 1),
                         value_ptr, Names::name(Names::Comment::VALUE, static_cast<std::string>(obj->getName())));
}

llvm::Value *CodeGenLLVM::emit_allocate_primitive(llvm::Value *val, const std::shared_ptr<Klass> &klass)
{
    // allocate
    auto *const obj = emit_new_inner(klass->klass());

    // record value
    auto *const val_ptr =
        __ CreateStructGEP(_data.class_struct(klass), obj, HeaderLayout::DispatchTable + 1,
                           Names::name(Names::Comment::VALUE, static_cast<std::string>(obj->getName())));
    __ CreateStore(val, val_ptr);

    return obj;
}

llvm::Value *CodeGenLLVM::emit_allocate_int(llvm::Value *val)
{
    return emit_allocate_primitive(val, _builder->klass(BaseClassesNames[BaseClasses::INT]));
}

llvm::Value *CodeGenLLVM::emit_load_bool(llvm::Value *bool_obj)
{
    return emit_load_primitive(bool_obj, _data.class_struct(_builder->klass(BaseClassesNames[BaseClasses::BOOL])));
}

llvm::Value *CodeGenLLVM::emit_in_scope(const std::shared_ptr<ast::ObjectExpression> &object,
                                        const std::shared_ptr<ast::Type> &object_type,
                                        const std::shared_ptr<ast::Expression> &expr, llvm::Value *initializer)
{
    _table.push_scope();

    const auto local_type = semant::Semant::exact_type(object_type, _current_class->_type);
    auto *const object_ptr_type = _data.class_struct(_builder->klass(local_type->_string))->getPointerTo();

    // allocate pointer to local variable
    auto *const local_val =
        __ CreateAlloca(object_ptr_type, nullptr, Names::name(Names::Comment::ALLOCA, object->_object));

    _table.add_symbol(object->_object, Symbol(local_val, local_type));

    if (!initializer)
    {
        // initial value for trivial type or null
        initializer = semant::Semant::is_trivial_type(object_type)
                          ? static_cast<llvm::Value *>(_data.init_value(object_type))
                          : static_cast<llvm::Value *>(llvm::ConstantPointerNull::get(object_ptr_type));
    }

    __ CreateStore(initializer, local_val);

    auto *const result = emit_expr(expr);
    _table.pop_scope();

    return result;
}

void CodeGenLLVM::emit_runtime_main()
{
    const auto &runtime_main =
        llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getInt32Ty(_context), {}, false),
                               llvm::Function::ExternalLinkage, static_cast<std::string>(RUNTIME_MAIN_FUNC), &_module);

    auto *entry = llvm::BasicBlock::Create(_context, Names::comment(Names::Comment::ENTRY_BLOCK), runtime_main);
    __ SetInsertPoint(entry);

    const auto main_klass = _builder->klass(MainClassName);
    auto *const main_object = emit_new_inner(main_klass->klass());

    const auto main_method = main_klass->method_full_name(MainMethodName);
    __ CreateCall(_module.getFunction(main_method), {main_object}, Names::name(Names::Comment::CALL, main_method));

    __ CreateRet(_int0_32);

    // TODO: verifier don't compile
    // GUARANTEE_DEBUG(!llvm::verifyFunction(*runtime_main));
}

#define EXIT_ON_ERROR(cond, error)                                                                                     \
    if (!cond)                                                                                                         \
    {                                                                                                                  \
        std::cerr << error << std::endl;                                                                               \
        exit(-1);                                                                                                      \
    }

void CodeGenLLVM::execute_linker(const std::string &object_file_name, const std::string &out_file_name)
{
    CODEGEN_VERBOSE_ONLY(LOG("Run linker for " + object_file_name + "."));

    const auto coolc_path = boost::dll::program_location().parent_path().string();
    const auto rt_lib_path =
        coolc_path + boost::filesystem::path::preferred_separator + static_cast<std::string>(RUNTIME_LIB_NAME);
    CODEGEN_VERBOSE_ONLY(LOG("Runtime library path: " + rt_lib_path));

    const auto clang_path = llvm::sys::findProgramByName(static_cast<std::string>(CLANG_EXE_NAME));
    EXIT_ON_ERROR(clang_path, "Can't find " + static_cast<std::string>(CLANG_EXE_NAME));
    CODEGEN_VERBOSE_ONLY(LOG(static_cast<std::string>(CLANG_EXE_NAME) + " library path: " + clang_path.get()));

    std::string error;
    // create executable
    EXIT_ON_ERROR((llvm::sys::ExecuteAndWait(
                       clang_path.get(),
                       {clang_path.get(), object_file_name, rt_lib_path, "-o", out_file_name}, // first arg is file name
                       llvm::None, {}, 0, 0, &error) == 0),
                  error);

    // delete object file
    std::filesystem::remove(object_file_name);

    CODEGEN_VERBOSE_ONLY(LOG("Finish linker for " + out_file_name + "."));
}

std::pair<std::string, std::string> CodeGenLLVM::find_best_vec_ext()
{
    if (!UseArchSpecFeatures)
    {
        return {"", "generic"};
    }

    const auto cpu = static_cast<std::string>(llvm::sys::getHostCPUName());

    llvm::StringMap<bool> features;
    llvm::sys::getHostCPUFeatures(features);

    const auto vec_ext_list =
#ifdef __x86_64__
        {"avx512f", "avx512vl", "avx2", "avx", "sse4.2", "sse4.1", "sse3", "sse2", "sse"};
#elif __aarch64__
        {""};
#else
        {""};
#endif

    for (const auto &ext : vec_ext_list)
    {
        const auto elem = features.find(ext);
        if (elem != features.end() && elem->getValue())
        {
            return {static_cast<std::string>(elem->getKey()), cpu};
        }
    }
    
    return {"", cpu};
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

    const auto arch_spec = find_best_vec_ext();

    CODEGEN_VERBOSE_ONLY(LOG("Target CPU: " + arch_spec.second));
    CODEGEN_VERBOSE_ONLY(LOG("Target Features: " + arch_spec.first));

    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();

    CODEGEN_VERBOSE_ONLY(LOG("Initialized llvm emitter."));

    std::string error;
    const auto *const target = llvm::TargetRegistry::lookupTarget(target_triple, error);
    EXIT_ON_ERROR(target, error);

    CODEGEN_VERBOSE_ONLY(LOG("Found target: " + std::string(target->getName())));

    auto *const target_machine = target->createTargetMachine(
        target_triple, arch_spec.second, arch_spec.first, llvm::TargetOptions(), llvm::Optional<llvm::Reloc::Model>());
    EXIT_ON_ERROR(target_machine, "Can't create target machine!");

    _module.setDataLayout(target_machine->createDataLayout());
    _module.setTargetTriple(target_triple);

    CODEGEN_VERBOSE_ONLY(LOG("Initialized target machine."));

    // open object file
    std::error_code ec;
    llvm::raw_fd_ostream dest(obj_file, ec);
    EXIT_ON_ERROR(!ec, "Could not open file: " + ec.message());

    llvm::legacy::PassManager pass;
    EXIT_ON_ERROR(!target_machine->addPassesToEmitFile(pass, dest, nullptr, llvm::CGFT_ObjectFile),
                  "TargetMachine can't emit a file of this type!");

    CODEGEN_VERBOSE_ONLY(LOG("Run llvm emitter."));

    pass.run(_module);
    dest.flush();
    dest.close();

    CODEGEN_VERBOSE_ONLY(LOG("Finished llvm emitter."));

    execute_linker(obj_file, out_file);
}