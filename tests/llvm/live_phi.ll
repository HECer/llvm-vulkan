; LV-PASSES: lv-dce
; LV-EXPECT: %live_phi = phi i32
; LV-EXPECT: %live_next = add i32 %live_phi, 1
; LV-EXPECT: %live_condition = icmp ult i32 %live_next, 9
; LV-REJECT: %dead_
; LV-EXIT: 0

define i32 @count() {
entry:
  %dead_entry = add i32 1, 2
  br label %loop
loop:
  %live_phi = phi i32 [ 0, %entry ], [ %live_next, %loop ]
  %dead_phi = phi i32 [ 5, %entry ], [ %dead_next, %loop ]
  %live_next = add i32 %live_phi, 1
  %dead_next = mul i32 %dead_phi, 7
  %live_condition = icmp ult i32 %live_next, 9
  br i1 %live_condition, label %loop, label %exit
exit:
  ret i32 %live_next
}

define i32 @main() {
  %a = call i32 @count()
  %ok = icmp eq i32 %a, 9
  %r = select i1 %ok, i32 0, i32 1
  ret i32 %r
}
