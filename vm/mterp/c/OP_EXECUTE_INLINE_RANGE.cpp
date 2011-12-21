HANDLE_OPCODE(OP_EXECUTE_INLINE_RANGE /*{vCCCC..v(CCCC+AA-1)}, inline@BBBB*/)
    {
#ifdef INLINE_ARG_EXPANDED
        u4 arg0, arg1, arg2, arg3, arg4, arg5, arg6;
        arg0 = arg1 = arg2 = arg3 = arg4 = arg5 = 0;      /* placate gcc */
        arg6 = 0;


        EXPORT_PC();

        vsrc1 = INST_AA(inst);      /* #of args */
        ref = FETCH(1);             /* inline call "ref" */
        vdst = FETCH(2);            /* range base */
        LOGE("|execute-inline-range args=%d @%d {regs=v%d-v%d}",
            vsrc1, ref, vdst, vdst+vsrc1-1);

        assert((vdst >> 16) == 0);  // 16-bit type -or- high 16 bits clear
        assert(vsrc1 <= 7);

        switch (vsrc1) {
        case 7:
            arg6 = GET_REGISTER(vdst+6);
            /* fall through */
        case 6:
            arg5 = GET_REGISTER(vdst+5);
            /* fall through */
        case 5:
            arg4 = GET_REGISTER(vdst+4);
            /* fall through */
        case 4:
            arg3 = GET_REGISTER(vdst+3);
            /* fall through */
        case 3:
            arg2 = GET_REGISTER(vdst+2);
            /* fall through */
        case 2:
            arg1 = GET_REGISTER(vdst+1);
            /* fall through */
        case 1:
            arg0 = GET_REGISTER(vdst+0);
            /* fall through */
        default:        // case 0
            ;
        }

        if (self->interpBreak.ctl.subMode & kSubModeDebuggerActive) {
            if (!dvmPerformInlineOp7Dbg(arg0, arg1, arg2, arg3, &retval, ref, arg4, arg5, arg6))
                GOTO_exceptionThrown();
        } else {
            if (!dvmPerformInlineOp7Std(arg0, arg1, arg2, arg3, &retval, ref, arg4, arg5, arg6))
                GOTO_exceptionThrown();
        }
#else //ifdef INLINE_ARG_EXPANDED
        u4 arg0, arg1, arg2, arg3;
        arg0 = arg1 = arg2 = arg3 = 0;      /* placate gcc */

        EXPORT_PC();

        vsrc1 = INST_AA(inst);      /* #of args */
        ref = FETCH(1);             /* inline call "ref" */
        vdst = FETCH(2);            /* range base */
        ILOGV("|execute-inline-range args=%d @%d {regs=v%d-v%d}",
            vsrc1, ref, vdst, vdst+vsrc1-1);

        assert((vdst >> 16) == 0);  // 16-bit type -or- high 16 bits clear
        assert(vsrc1 <= 4);

        switch (vsrc1) {
        case 4:
            arg3 = GET_REGISTER(vdst+3);
            /* fall through */
        case 3:
            arg2 = GET_REGISTER(vdst+2);
            /* fall through */
        case 2:
            arg1 = GET_REGISTER(vdst+1);
            /* fall through */
        case 1:
            arg0 = GET_REGISTER(vdst+0);
            /* fall through */
        default:        // case 0
            ;
        }

        if (self->interpBreak.ctl.subMode & kSubModeDebuggerActive) {
            if (!dvmPerformInlineOp4Dbg(arg0, arg1, arg2, arg3, &retval, ref))
                GOTO_exceptionThrown();
        } else {
            if (!dvmPerformInlineOp4Std(arg0, arg1, arg2, arg3, &retval, ref))
                GOTO_exceptionThrown();
        }
#endif //ifdef INLINE_ARG_EXPANDED
    }
    FINISH(3);
OP_END
