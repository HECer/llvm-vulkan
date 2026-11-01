; LV-PASSES: lv-pipeline
; LV-EXPECT: ret i32 42
; LV-EXPECT: !DICompileUnit
; LV-REJECT: %fold_me =
; LV-EXIT: 42

source_filename = "debug_example.c"

define i32 @main() !dbg !5 {
entry:
  %fold_me = add i32 20, 22, !dbg !9
  ret i32 %fold_me, !dbg !10
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!3, !4}
!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "LLVM Vulkan tests", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "debug_example.c", directory: "/examples")
!2 = !{}
!3 = !{i32 7, !"Dwarf Version", i32 4}
!4 = !{i32 2, !"Debug Info Version", i32 3}
!5 = distinct !DISubprogram(name: "main", scope: !1, file: !1, line: 1, type: !6, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !2)
!6 = !DISubroutineType(types: !7)
!7 = !{!8}
!8 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!9 = !DILocation(line: 2, column: 7, scope: !5)
!10 = !DILocation(line: 3, column: 3, scope: !5)
