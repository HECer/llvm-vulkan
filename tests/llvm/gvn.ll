; LV-PASSES: lv-gvn
; LV-EXPECT: %first = add i32 %x, %y
; LV-EXPECT: %sum = mul i32 %first, %first
; LV-REJECT: %keep_other_block = add i32 %x, %y
; LV-REJECT: %comm_other_block = add i32 %y, %x
; LV-EXIT: 0

define i32 @calculate(i32 %x, i32 %y) {
entry:
  %first = add i32 %x, %y
  %sum = mul i32 %first, %first
  br label %next

next:
  %keep_other_block = add i32 %x, %y
  %comm_other_block = add i32 %y, %x
  %res = add i32 %keep_other_block, %comm_other_block
  ret i32 %res
}

define i32 @main() {
  %a = call i32 @calculate(i32 2, i32 3)
  ; %first = 5
  ; %keep_other_block replaced by %first (5)
  ; %comm_other_block replaced by %first (5)
  ; %res = 5 + 5 = 10
  %ok = icmp eq i32 %a, 10
  %r = select i1 %ok, i32 0, i32 1
  ret i32 %r
}
