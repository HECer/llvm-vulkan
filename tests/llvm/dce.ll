; LV-PASSES: lv-dce
; LV-EXPECT: store volatile i32 7
; LV-EXPECT: %keep_load = load volatile i32
; LV-EXPECT: %keep_atomic = atomicrmw add
; LV-EXPECT: call void @side_effect()
; LV-EXPECT: %keep_value = add i32 %x, 1
; LV-EXPECT: %keep_load_unused = load i32
; LV-EXPECT: loop:
; LV-REJECT: %dead_

@data = global i32 0
declare void @side_effect()

define i32 @effects(i32 %x) {
entry:
  %dead_a = add i32 %x, 11
  %dead_b = xor i32 %dead_a, 64
  %keep_value = add i32 %x, 1
  store volatile i32 7, ptr @data
  %keep_load = load volatile i32, ptr @data
  %keep_load_unused = load i32, ptr @data
  %keep_atomic = atomicrmw add ptr @data, i32 1 seq_cst
  call void @side_effect()
  ret i32 %keep_value
}

; Dead phi/add cycles may disappear, but the potentially nonterminating loop
; itself must remain; a plain instruction-liveness pass cannot remove it.
define void @dead_cycle(i1 %stop) {
entry:
  br label %loop
loop:
  %dead_phi = phi i32 [ 0, %entry ], [ %dead_next, %loop ]
  %dead_next = add i32 %dead_phi, 1
  br i1 %stop, label %exit, label %loop
exit:
  ret void
}
