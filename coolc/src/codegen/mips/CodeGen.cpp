#include "codegen/mips/CodeGen.h"

using namespace codegen;

#define __ _asm.

CodeGen::CodeGen(const std::shared_ptr<semant::ClassNode> &root)
    : _table([](const std::string &name, const Symbol &s) {
          CODEGEN_VERBOSE_ONLY(LOG("Add symbol " + name + " with type " +
                                   ((s._type == Symbol::FIELD) ? "FIELD" : "LOCAL") + " and offset " +
                                   std::to_string(s._offset)));
      }),
      _builder(root), _root(root), _data(_builder), _asm(_code), _a0(_asm, Register::$a0), _s0(_asm, Register::$s0),
      _equality_test_label(_asm, "equality_test",
                           Label::ALLOW_NO_BIND), // don't check label binding during verification
      _object_copy_label(_asm, "Object.copy", Label::ALLOW_NO_BIND),
      _case_abort_label(_asm, "_case_abort", Label::ALLOW_NO_BIND),
      _case_abort2_label(_asm, "_case_abort2", Label::ALLOW_NO_BIND),
      _dispatch_abort_label(_asm, "_dispatch_abort", Label::ALLOW_NO_BIND),
      _gen_gc_assign_label(_asm, "_GenGC_Assign", Label::ALLOW_NO_BIND) // GC
{
    // here we start generate methods code
    __ text_section();

    // we export some structures and method to global
    __ global(Label(_asm, "Main_init"));
    __ global(Label(_asm, "Int_init"));
    __ global(Label(_asm, "String_init"));
    __ global(Label(_asm, "Bool_init"));
    __ global(Label(_asm, "Main.main"));
}

CodeBuffer CodeGen::emit()
{
    emit_class_code(_root);

    CodeBuffer data = _data.emit();

    Assembler::cross_resolve_labels(_asm, _data.get_asm()); // verify that all labels were binded
    return data += _code;
}

void CodeGen::emit_class_code(const std::shared_ptr<semant::ClassNode> &node)
{
    _current_class = node->_class;
    CODEGEN_VERBOSE_ONLY(LOG_ENTER("GEN CLASS " + _current_class->_type->_string));

    _table.push_scope();
    add_fields_to_table(_current_class);    // add fields to table, setup ClassCode for prototype emitting
    emit_class_init_method(_current_class); // emit init method
    // emit methods. Must be after add_fields_to_table, because this call add class fields
    if (semant::Semant::is_not_basic_class(_current_class->_type))
    {
        // it is real methods
        std::for_each(_current_class->_features.begin(), _current_class->_features.end(), [&](const auto &feature) {
            if (std::holds_alternative<ast::MethodFeature>(feature->_base))
            {
                emit_class_method(feature);
            }
        });
    }
    else
    {
        // it is dummies for basic classes. There are external symbols
        std::for_each(_current_class->_features.begin(), _current_class->_features.end(), [&](const auto &feature) {
            // just register them in asm
            if (std::holds_alternative<ast::MethodFeature>(feature->_base))
            {
                Label(_asm, _builder.klass(_current_class->_type->_string)->method_full_name(feature->_object->_object),
                      Label::ALLOW_NO_BIND);
            }
        });
    }

    // process children
    std::for_each(node->_children.begin(), node->_children.end(), [&](const auto &node) { emit_class_code(node); });

    _table.pop_scope();

    CODEGEN_VERBOSE_ONLY(LOG_EXIT("GEN CLASS " + _current_class->_type->_string));
}

void CodeGen::add_fields_to_table(const std::shared_ptr<ast::Class> &klass)
{
    const auto &this_klass = _builder.klass(klass->_type->_string);
    for (auto field = this_klass->fields_begin(); field != this_klass->fields_end(); field++)
    {
        const std::string &name = (*field)->_object->_object;
        const auto offset = this_klass->field_offset(field - this_klass->fields_begin());
        // save fields to local symbol table for further codegen
        Symbol s(Symbol::FIELD, offset);
        _table.add_symbol(name, s);
    }
}

void CodeGen::emit_binary_expr(const ast::BinaryExpression &expr)
{
    CODEGEN_VERBOSE_ONLY(LOG_ENTER("GEN BINARY EXPR"));

    emit_expr(expr._lhs);
    // we hope to see the first argument in acc
    __ push(_a0);
    emit_expr(expr._rhs);

    bool logical_result = false;
    const Register t1(_asm, Register::$t1); // allocate new temp reg
    const Register t2(_asm, Register::$t2);
    const Register t5(_asm, Register::$t5);

    // rhs in acc, load lhs to t0
    __ pop(t1);
    // for arith operations create copy of the rhs for result
    __ move(t2, _a0);
    // operand are in t2 and t1, use t5 for result
    if (!std::holds_alternative<ast::EqExpression>(expr._base))
    {
        emit_load_int(t2, t2); // load real values
        emit_load_int(t1, t1);

        std::visit(ast::overloaded{[&](const ast::MinusExpression &minus) { __ sub(t5, t1, t2); },
                                   [&](const ast::PlusExpression &plus) { __ add(t5, t1, t2); },
                                   [&](const ast::DivExpression &div) { __ div(t5, t1, t2); },
                                   [&](const ast::MulExpression &mul) { __ mul(t5, t1, t2); },
                                   [&](const ast::LTExpression &lt) {
                                       // trait it as arith result
                                       __ slt(t5, t1, t2);
                                       __ la(_a0, _data.declare_bool_const(false));
                                   },
                                   [&](const ast::LEExpression &le) {
                                       logical_result = true;

                                       const Label true_label(_asm, NameSpace::true_branch());
                                       const Label end_label(_asm, NameSpace::merge_block());

                                       __ ble(t1, t2, true_label);                  // t1 < a0
                                       __ la(_a0, _data.declare_bool_const(false)); // false -> a0 if false
                                       __ j(end_label);                             // jump to end

                                       {
                                           const AssemblerMarkSection mark(_asm, true_label); // true path
                                           __ la(_a0, _data.declare_bool_const(true));
                                       }

                                       const AssemblerMarkSection mark(_asm, end_label); // jump here to continue
                                   },
                                   [&](const ast::EqExpression &le) {}},
                   expr._base);
    }
    else
    {
        logical_result = true;

        const Label equal_refs_label(_asm, NameSpace::true_branch());
        const Label equal_primitive_vals_label(_asm, NameSpace::true_branch());
        const Label equal_end_label(_asm, NameSpace::merge_block());

        const Register a1(_asm, Register::$a1);

        __ beq(t2, t1, equal_refs_label); // are they the same reference?
        // no, they dont have the same reference
        __ la(_a0, _data.declare_bool_const(true)); // if no, set a0 to true, a1 to false
        __ la(a1, _data.declare_bool_const(false));
        __ jal(_equality_test_label);                                // in a0 expect a0 if the same and a1 if false
        emit_load_bool(_a0, _a0);                                    // real value in a0
        __ beq(_a0, _data.true_value(), equal_primitive_vals_label); // do they have same type and value?
        // no, they dont have the same type and value
        __ la(_a0, _data.declare_bool_const(false)); // end of false branch
        __ j(equal_end_label);                       // jump to end

        {
            // yes, they have the same reference
            const AssemblerMarkSection mark(_asm, equal_refs_label);
            __ la(_a0, _data.declare_bool_const(true)); // end of true branch
            __ j(equal_end_label);                      // jump to end
        }

        {
            // yes, they have the same type and value
            const AssemblerMarkSection mark(_asm, equal_primitive_vals_label);
            __ la(_a0, _data.declare_bool_const(true));
        }

        const AssemblerMarkSection mark(_asm, equal_end_label); // jump here to continue
    }

    // object in a0, result in t5
    // create object and set field
    __ jal(_object_copy_label);
    // result in a0
    if (!logical_result)
    {
        emit_store_int(_a0, t5);
    }

    CODEGEN_VERBOSE_ONLY(LOG_EXIT("GEN BINARY EXPR"));
}

void CodeGen::emit_unary_expr(const ast::UnaryExpression &expr)
{
    CODEGEN_VERBOSE_ONLY(LOG_ENTER("GEN UNAXRY EXPR"));

    emit_expr(expr._expr);

    bool logical_result = false;
    // result in acc
    const Register t5(_asm, Register::$t5);
    std::visit(ast::overloaded{[&](const ast::IsVoidExpression &isvoid) {
                                   logical_result = true;

                                   const Label its_void_label(_asm, NameSpace::true_branch());
                                   const Label end_label_label(_asm, NameSpace::merge_block());

                                   __ beq(_a0, __ zero(), its_void_label);
                                   // false branch
                                   __ la(_a0, _data.declare_bool_const(false)); // end of false branch
                                   __ j(end_label_label);                       // jump to end
                                   // true branch
                                   {
                                       const AssemblerMarkSection mark(_asm, its_void_label);
                                       __ la(_a0, _data.declare_bool_const(true)); // end of true branch
                                   }

                                   const AssemblerMarkSection mark(_asm, end_label_label);
                               },
                               [&](const ast::NotExpression &) {
                                   // trait it as arith result
                                   __ move(t5, _a0);
                                   emit_load_bool(t5, t5);
                                   __ xori(t5, t5, _data.true_value());
                                   __ la(_a0, _data.declare_bool_const(false));
                               },
                               [&](const ast::NegExpression &neg) {
                                   __ move(t5, _a0);
                                   emit_load_int(t5, t5);
                                   __ sub(t5, __ zero(), t5);
                               }},
               expr._base);
    // object in a0, result in t5
    // create object and set field
    __ jal(_object_copy_label);
    // result in a0
    if (!logical_result)
    {
        emit_store_int(_a0, t5);
    }

    CODEGEN_VERBOSE_ONLY(LOG_EXIT("GEN UNAXRY EXPR"));
}

void CodeGen::emit_expr(const std::shared_ptr<ast::Expression> &expr)
{
    std::visit(ast::overloaded{[&](ast::BoolExpression &bool_expr) { emit_bool_expr(bool_expr); },
                               [&](ast::StringExpression &str) { emit_string_expr(str); },
                               [&](ast::IntExpression &number) { emit_int_expr(number); },
                               [&](ast::ObjectExpression &object) { emit_object_expr(object); },
                               [&](ast::BinaryExpression &binary_expr) { emit_binary_expr(binary_expr); },
                               [&](ast::UnaryExpression &unary_expr) { emit_unary_expr(unary_expr); },
                               [&](ast::NewExpression &alloc) { emit_new_expr(alloc); },
                               [&](ast::CaseExpression &branch) { emit_cases_expr(branch); },
                               [&](ast::LetExpression &let) { emit_let_expr(let); },
                               [&](ast::ListExpression &list) { emit_list_expr(list); },
                               [&](ast::WhileExpression &loop) { emit_loop_expr(loop); },
                               [&](ast::IfExpression &branch) { emit_if_expr(branch); },
                               [&](ast::DispatchExpression &dispatch) { emit_dispatch_expr(dispatch); },
                               [&](ast::AssignExpression &assign) { emit_assign_expr(assign); }},
               expr->_data);
}

void CodeGen::emit_method_prologue()
{
    __ addiu(__ sp(), __ sp(), -(WORD_SIZE * 3));
    __ sw(__ fp(), __ sp(), (WORD_SIZE * 3));
    __ sw(_s0, __ sp(), (WORD_SIZE * 2));
    __ sw(__ ra(), __ sp(), WORD_SIZE);
    __ addiu(__ fp(), __ sp(), WORD_SIZE * 3); // 4 for fp, 4 fo ra, 4 for s0
    __ move(_s0, _a0);                         // "this" in a0

    _asm.set_sp_offset(-(WORD_SIZE * 3));
}

void CodeGen::emit_method_epilogue(const int &params_num)
{
    __ lw(__ ra(), __ sp(), WORD_SIZE);
    __ lw(_s0, __ sp(), (2 * WORD_SIZE));
    __ lw(__ fp(), __ sp(), (3 * WORD_SIZE));
    __ addiu(__ sp(), __ sp(), (params_num + 3) * WORD_SIZE); // pop all arguments to preserve stack
    __ jr(__ ra());

    _asm.set_sp_offset(0);
}

void CodeGen::emit_class_init_method(const std::shared_ptr<ast::Class> &klass)
{
    CODEGEN_VERBOSE_ONLY(LOG_ENTER("GEN INIT METHOD FOR CLASS " + _current_class->_type->_string));

    const AssemblerMarkSection mark(_asm, Label(_asm, NameSpace::init_method(_builder.klass(klass->_type->_string))));

    emit_method_prologue();

    if (!semant::Semant::is_empty_type(klass->_parent)) // Object moment
    {
        __ jal(Label(_asm,
                     NameSpace::init_method(_builder.klass(
                         klass->_parent->_string)))); // receiver already is in acc, call parent constructor
    }

    std::for_each(klass->_features.begin(), klass->_features.end(), [&](const auto &feature) {
        if (std::holds_alternative<ast::AttrFeature>(feature->_base))
        {
            if (feature->_expr)
            {
                emit_expr(feature->_expr); // calculate expression

                const auto &this_field = _table.symbol(feature->_object->_object);
                assert(this_field._type == Symbol::FIELD); // impossible

                __ sw(_a0, _s0, this_field._offset); // save result to field in object
            }
        }
    });

    __ move(_a0, _s0); // return "this"
    emit_method_epilogue(0);

    CODEGEN_VERBOSE_ONLY(LOG_EXIT("GEN INIT METHOD FOR CLASS " + _current_class->_type->_string));
}

void CodeGen::emit_class_method(const std::shared_ptr<ast::Feature> &method)
{
    CODEGEN_VERBOSE_ONLY(LOG_ENTER("GEN METHOD " + method->_object->_object));

    const AssemblerMarkSection mark(
        _asm, Label(_asm, _builder.klass(_current_class->_type->_string)->method_full_name(method->_object->_object)));

    emit_method_prologue();

    // add formals to symbol table
    _table.push_scope();
    const auto &params = std::get<ast::MethodFeature>(method->_base)._formals;
    const int params_num = params.size();
    for (int i = 0; i < params_num; i++)
    {
        const std::string &name = params[i]->_object->_object;
        const uint32_t offset = (params_num - i) * WORD_SIZE;

        Symbol s(Symbol::LOCAL, offset);
        _table.add_symbol(params[i]->_object->_object, s); // map formal parameters to arguments
    }

    emit_expr(method->_expr);
    _table.pop_scope();

    emit_method_epilogue(params_num);

    CODEGEN_VERBOSE_ONLY(LOG_EXIT("GEN METHOD " + method->_object->_object));
}

void CodeGen::emit_bool_expr(const ast::BoolExpression &expr)
{
    __ la(_a0, _data.declare_bool_const(expr._value));
}

void CodeGen::emit_int_expr(const ast::IntExpression &expr)
{
    __ la(_a0, _data.declare_int_const(expr._value));
}

void CodeGen::emit_string_expr(const ast::StringExpression &expr)
{
    __ la(_a0, _data.declare_string_const(expr._string));
}

void CodeGen::emit_object_expr(const ast::ObjectExpression &expr)
{
    CODEGEN_VERBOSE_ONLY(LOG_ENTER("GEN OBJECT EXPR FOR " + expr._object));

    if (!semant::Scope::can_assign(expr._object))
    {
        __ move(_a0, _s0); // self object: just copy to acc

        CODEGEN_VERBOSE_ONLY(LOG_EXIT("GEN OBJECT EXPR FOR " + expr._object));
        return;
    }

    const auto &object = _table.symbol(expr._object);
    if (object._type == Symbol::FIELD)
    {
        __ lw(_a0, _s0, object._offset); // object field
    }
    else
    {
        __ lw(_a0, __ fp(), object._offset); // local object defined in let, case, formal parameter
    }

    CODEGEN_VERBOSE_ONLY(LOG_EXIT("GEN OBJECT EXPR FOR " + expr._object));
}

void CodeGen::emit_new_expr(const ast::NewExpression &expr)
{
    CODEGEN_VERBOSE_ONLY(LOG_ENTER("GEN NEW EXPR"));

    // we know the type
    if (!semant::Semant::is_self_type(expr._type))
    {
        const auto &klass = _builder.klass(expr._type->_string);

        __ la(_a0, Label(_asm, NameSpace::prototype(klass)));
        __ jal(_object_copy_label);                         // result in acc
        __ jal(Label(_asm, NameSpace::init_method(klass))); // result in acc
    }
    else
    {
        // we dont know the type
        Register t5(_asm, Register::$t5);
        Register t6(_asm, Register::$t6);

        __ lw(t6, _s0, 0);                                       // load tag of "this"
        __ sll(t5, t6, OBJECT_HEADER_SIZE_IN_BYTES / WORD_SIZE); // calculate offset in object table
        __ la(t6, _data.class_obj_tab());                        // load object table
        __ addu(t5, t6, t5);                                     // find protobj position in table

        __ lw(_a0, t5, 0);
        __ jal(_object_copy_label); // result in acc

        __ lw(t5, t5, WORD_SIZE); // next slot is init method
        __ jalr(t5);
    }

    CODEGEN_VERBOSE_ONLY(LOG_EXIT("GEN NEW EXPR"));
}

void CodeGen::emit_in_scope(const std::shared_ptr<ast::ObjectExpression> &object,
                            const std::shared_ptr<ast::Type> &object_type, const std::shared_ptr<ast::Expression> &expr,
                            const bool &assign_acc)
{
    _table.push_scope();

    Symbol s(Symbol::LOCAL, __ sp_offset());
    _table.add_symbol(object->_object, s);

    if (assign_acc)
    {
        __ push(_a0); // save acc to new slot
    }
    else
    {
        if (semant::Semant::is_trivial_type(object_type))
        {
            __ la(_a0, _data.emit_init_value(object_type));
            __ push(_a0);
        }
        else
        {
            __ push(__ zero()); // defualt value for uninitialized objects
        }
    }

    emit_expr(expr);

    _table.pop_scope();
    __ pop(); // delete slot
}

void CodeGen::emit_cases_expr(const ast::CaseExpression &expr)
{
    CODEGEN_VERBOSE_ONLY(LOG_ENTER("GEN CASE EXPR"));

    emit_expr(expr._expr);

    std::string case_branch_name = NameSpace::true_branch();
    const std::string no_branch_name = NameSpace::false_branch();
    const Label continue_label(_asm, NameSpace::merge_block());

    // in brackets because we want to deallocate t1 before next expressions emitting
    {
        const Register t1(_asm, Register::$t1);

        __ bne(_a0, __ zero(), Label(_asm, case_branch_name)); // is expression void
        // yes, it is void
        __ la(_a0, _data.declare_string_const(_current_class->_file_name)); // save file name to a0
        __ li(t1, expr._expr->_line_number);                                // save line number to t1
        __ jal(_case_abort2_label);                                         // abort
    }

    // we want to generate code for the the most precise cases first, so sort cases by tag
    auto cases = expr._cases;
    std::sort(cases.begin(), cases.end(),
              [&](const std::shared_ptr<ast::Case> &case_a, const std::shared_ptr<ast::Case> &case_b) {
                  return _builder.tag(case_b->_type->_string) < _builder.tag(case_a->_type->_string);
              });

    // no, it is not void
    // Last case is a special case: branch to abort
    for (int i = 0; i < cases.size(); i++)
    {
        const AssemblerMarkSection mark(_asm, Label(_asm, case_branch_name));
        {
            const Register t1(_asm, Register::$t1);

            __ lw(t1, _a0, 0); // if we here, so object is in acc. Load its tag to t1

            case_branch_name = (i < cases.size() - 1 ? NameSpace::true_branch() : no_branch_name);

            const auto &klass = _builder.klass(cases[i]->_type->_string);

            __ blt(
                t1, klass->tag(),
                Label(_asm,
                      case_branch_name)); // if object tag lower than the lowest tag for this branch, jump to next case
            __ bgt(
                t1, klass->child_max_tag(),
                Label(
                    _asm,
                    case_branch_name)); // if object tag higher that the highest tag for this branch, jump to next case
        }
        // this branch is ok. Object is in acc
        emit_in_scope(cases[i]->_object, cases[i]->_type, cases[i]->_expr);
        __ j(continue_label);
    }

    // does not find suitable branch

    {
        const AssemblerMarkSection mark(_asm, Label(_asm, no_branch_name));
        __ jal(_case_abort_label); // abort
    }

    const AssemblerMarkSection mark(_asm, continue_label);

    CODEGEN_VERBOSE_ONLY(LOG_EXIT("GEN CASE EXPR"));
}

void CodeGen::emit_let_expr(const ast::LetExpression &expr)
{
    CODEGEN_VERBOSE_ONLY(LOG_ENTER("GEN LET EXPR"));

    if (expr._expr)
    {
        emit_expr(expr._expr); // result in acc
    }

    emit_in_scope(expr._object, expr._type, expr._body_expr, expr._expr != nullptr);

    CODEGEN_VERBOSE_ONLY(LOG_EXIT("GEN LET EXPR"));
}

void CodeGen::emit_list_expr(const ast::ListExpression &expr)
{
    CODEGEN_VERBOSE_ONLY(LOG_ENTER("GEN LIST EXPR"));

    std::for_each(expr._exprs.begin(), expr._exprs.end(), [&](const auto &e) {
        emit_expr(e); // last expression value will be in acc
    });

    CODEGEN_VERBOSE_ONLY(LOG_EXIT("GEN LIST EXPR"));
}

void CodeGen::emit_branch_to_label_if_false(const Label &label)
{
    emit_load_bool(_a0, _a0);
    __ beq(_a0, DataSection::false_value(), label);
}

void CodeGen::emit_loop_expr(const ast::WhileExpression &expr)
{
    CODEGEN_VERBOSE_ONLY(LOG_ENTER("GEN LOOP EXPR"));

    const Label loop_header_label(_asm, NameSpace::loop_header());
    const Label loop_tail_label(_asm, NameSpace::loop_tail());

    {
        const AssemblerMarkSection mark(_asm, loop_header_label);

        emit_expr(expr._predicate); // result in acc
        emit_branch_to_label_if_false(loop_tail_label);
        // loop body
        emit_expr(expr._body_expr);
        __ j(loop_header_label); // go to loop start
    }

    const AssemblerMarkSection mark(_asm, loop_tail_label); // continue

    CODEGEN_VERBOSE_ONLY(LOG_EXIT("GEN LOOP EXPR"));
}

void CodeGen::emit_if_expr(const ast::IfExpression &expr)
{
    CODEGEN_VERBOSE_ONLY(LOG_ENTER("GEN IF EXPR"));

    const Label false_branch_label(_asm, NameSpace::false_branch());
    const Label continue_label(_asm, NameSpace::merge_block());

    emit_expr(expr._predicate); // result in acc
    emit_branch_to_label_if_false(false_branch_label);
    // true branch
    emit_expr(expr._true_path_expr);
    __ j(continue_label); // continue execution

    {
        // false branch
        const AssemblerMarkSection mark(_asm, false_branch_label);

        emit_expr(expr._false_path_expr);
    }

    const AssemblerMarkSection mark(_asm, continue_label);

    CODEGEN_VERBOSE_ONLY(LOG_EXIT("GEN IF EXPR"));
}

void CodeGen::emit_dispatch_expr(const ast::DispatchExpression &expr)
{
    CODEGEN_VERBOSE_ONLY(LOG_ENTER("GEN DISPATCH EXPR"));

    // put all args on stack. Callee have to get rid of them
    const int args_num = expr._args.size();
    // allocate space
    __ addiu(__ sp(), __ sp(), -(args_num * WORD_SIZE));
    for (int i = 0; i < args_num; i++)
    {
        emit_expr(expr._args[i]);
        __ sw(_a0, __ sp(), (args_num - i) * WORD_SIZE);
    }

    emit_expr(expr._expr); // receiver in acc

    const Label dispatch_to_void_label(_asm, NameSpace::true_branch());
    const Register t1(_asm, Register::$t1);
    __ beq(_a0, __ zero(), dispatch_to_void_label);

    // not void
    std::visit(
        ast::overloaded{
            [&](const ast::ObjectDispatchExpression &disp) {
                __ lw(t1, _a0, DISPATCH_TABLE_OFFSET); // load dispatch table
                __ lw(t1, t1,
                      _builder.klass(semant::Semant::exact_type(expr._expr->_type, _current_class->_type)->_string)
                          ->method_offset(expr._object->_object)); // load method label
                __ jalr(t1);                                       // jump to method
            },
            [&](const ast::StaticDispatchExpression &disp) {
                // we know exactly method name
                __ jal(Label(_asm, _builder.klass(disp._type->_string)->method_full_name(expr._object->_object)));
            }},
        expr._base);
    const Label continue_label(_asm, NameSpace::merge_block());
    __ j(continue_label);

    // void
    {
        const AssemblerMarkSection mark(_asm, dispatch_to_void_label);
        __ li(t1, expr._expr->_line_number);
        __ la(_a0, _data.declare_string_const(_current_class->_file_name));
        __ jal(_dispatch_abort_label);
    }

    const AssemblerMarkSection mark(_asm, continue_label);

    CODEGEN_VERBOSE_ONLY(LOG_EXIT("GEN DISPATCH EXPR"));
}

void CodeGen::emit_assign_expr(const ast::AssignExpression &expr)
{
    CODEGEN_VERBOSE_ONLY(LOG_ENTER("GEN ASSIGN EXPR"));

    emit_expr(expr._expr); // result in acc
    const auto &symbol = _table.symbol(expr._object->_object);

    if (symbol._type == Symbol::FIELD)
    {
        __ sw(_a0, _s0, symbol._offset);
        emit_gc_update(_s0, symbol._offset);
    }
    else
    {
        __ sw(_a0, __ fp(), symbol._offset);
    }

    CODEGEN_VERBOSE_ONLY(LOG_EXIT("GEN ASSIGN EXPR"));
}

void CodeGen::emit_load_int(const Register &int_obj, const Register &int_val)
{
    __ lw(int_val, int_obj, OBJECT_HEADER_SIZE_IN_BYTES);
}

void CodeGen::emit_store_int(const Register &int_obj, const Register &int_val)
{
    __ sw(int_val, int_obj, OBJECT_HEADER_SIZE_IN_BYTES);
    emit_gc_update(int_obj, OBJECT_HEADER_SIZE_IN_BYTES);
}

void CodeGen::emit_load_bool(const Register &bool_obj, const Register &bool_val)
{
    emit_load_int(bool_obj, bool_val);
}

void CodeGen::emit_store_bool(const Register &bool_obj, const Register &bool_val)
{
    emit_store_int(bool_obj, bool_val);
}

void CodeGen::emit_gc_update(const Register &obj, const int &offset)
{
    Register a1(_asm, Register::$a1);
    __ addiu(a1, obj, offset);
    __ jal(_gen_gc_assign_label);
}