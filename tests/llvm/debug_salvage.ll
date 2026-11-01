; LV-PASSES: lv-dce
; LV-EXPECT: !DILocalVariable(name: "intermediate"
; LV-EXPECT: !DIExpression(DW_OP_plus_uconst, 1, DW_OP_stack_value)
; LV-REJECT: %dead_value =
; LV-EXIT: 0

source_filename = "debug_salvage.c"
declare void @llvm.dbg.value(metadata, metadata, metadata)

define i32 @with_debug(i32 %x) !dbg !5 {
entry:
  %dead_value = add i32 %x, 1, !dbg !10
  call void @llvm.dbg.value(metadata i32 %dead_value, metadata !9, metadata !DIExpression()), !dbg !10
  ret i32 %x, !dbg !11
}

define i32 @main() {
  %a = call i32 @with_debug(i32 0)
  ret i32 %a
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!3, !4}
!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "LLVM Vulkan tests", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "debug_salvage.c", directory: "/examples")
!2 = !{}
!3 = !{i32 7, !"Dwarf Version", i32 4}
!4 = !{i32 2, !"Debug Info Version", i32 3}
!5 = distinct !DISubprogram(name: "with_debug", scope: !1, file: !1, line: 1, type: !6, scopeLine: 1, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !2)
!6 = !DISubroutineType(types: !7)
!7 = !{!8, !8}
!8 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!9 = !DILocalVariable(name: "intermediate", scope: !5, file: !1, line: 2, type: !8)
!10 = !DILocation(line: 2, column: 7, scope: !5)
!11 = !DILocation(line: 3, column: 3, scope: !5)
