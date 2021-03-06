#include "tests-support.hpp"

#define SMALL_LINKED_LIST 15700
#define MEDIUM_LINKED_LIST 8038400
#define BIG_LINKED_LIST 16076800

template <class GCType, int heap_size, int nodes, bool verbose, int verbose_iter> DECLARE_TEST(linked_list_allocation)
{
    size_t dataf = LLNODE.offset(0);
    size_t nextf = LLNODE.offset(1);

    size_t intf = INTOBJ.offset(0);

    GCType gc(heap_size);

    {
        gc::StackRecord sr(&gc);

        // 1. allocate root nodes
        address root = ALLOCATE(LLNODE);
        address prev = root;
        sr.reg_root(&root);
        sr.reg_root(&prev);

        address curr = ALLOCATE(LLNODE);
        sr.reg_root(&curr);

        // link root and the current_node
        WRITE(root, nextf, curr);

        // link root and the int 0
        address root_i = ALLOCATE(INTOBJ);
        WRITE(root, dataf, root_i);
        WRITE(root_i, intf, (doubleword)0);

        // link current_node and the int 1
        address curr_i = ALLOCATE(INTOBJ);
        WRITE(curr, dataf, curr_i);
        WRITE(curr_i, intf, (doubleword)1);

        // 2. Allocate other nodes
        doubleword prev_val = 0;
        doubleword cur_val = 1;
        for (size_t i = 0; i < nodes; i++)
        {
            if constexpr (verbose)
            {
                if (i % verbose_iter == 0)
                {
                    std::cout << "Iteration: " << i << std::endl;
                }
            }

            gc::StackRecord sr1(&sr);

            // some slots for locals...
            address new_node = NULL;
            address new_int = NULL;
            address check_val = NULL;
            sr1.reg_root(&new_node);
            sr1.reg_root(&new_int);
            sr1.reg_root(&check_val);

            // allocate necessary nodes
            new_node = ALLOCATE(LLNODE);
            new_int = ALLOCATE(INTOBJ);
            check_val = ALLOCATE(INTOBJ); // rubbish
            check_val = ALLOCATE(INTOBJ);

            // calculate a new value
            doubleword next_val = READ_DW(READ_ADDRESS(prev, dataf), intf);
            next_val += READ_DW(READ_ADDRESS(curr, dataf), intf);

            // write new value to the new int
            WRITE(new_int, intf, next_val);

            // write a new int to the new node
            WRITE(new_node, dataf, new_int);

            // allocate some rubbish
            doubleword good_next_val = prev_val + cur_val;
            prev_val = cur_val;
            cur_val = good_next_val;
            WRITE(check_val, intf, cur_val);

            // link current node and the new one
            WRITE(curr, nextf, new_node);

            // update previous and current nodes
            prev = curr;
            curr = new_node;

            guarantee_eq(READ_DW(READ_ADDRESS(new_node, dataf), intf), READ_DW(check_val, intf));
        }
    }
}