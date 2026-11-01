; LV-PASSES: lv-pipeline
; LV-EXPECT: ret i32 0
; LV-REJECT: %fold_condition
; LV-REJECT: %dead_value
; LV-REJECT: fail:
; LV-EXIT: 0

define i32 @main() {
entry:
  %fold_condition = icmp slt i32 -1, 3
  br i1 %fold_condition, label %pass, label %fail
pass:
  %dead_value = mul i32 3, 9
  br label %exit
fail:
  br label %exit
exit:
  %answer = phi i32 [ 0, %pass ], [ 1, %fail ]
  ret i32 %answer
}
