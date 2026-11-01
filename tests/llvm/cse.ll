; LV-PASSES: lv-cse
; LV-EXPECT: %first = add i32 %x, %y
; LV-EXPECT: %sum = mul i32 %first, %first
; LV-EXPECT: %keep_flags = add nsw i32 %x, %y
; LV-EXPECT: %keep_freeze_a = freeze i32 poison
; LV-EXPECT: %keep_freeze_b = freeze i32 poison
; LV-EXPECT: %keep_load_a = load i32
; LV-EXPECT: %keep_load_b = load i32
; LV-EXPECT: %keep_other_block = add i32 %x, %y
; LV-REJECT: %duplicate =
; LV-EXIT: 0

@data = global i32 1

define i32 @calculate(i32 %x, i32 %y) {
entry:
  %first = add i32 %x, %y
  %duplicate = add i32 %x, %y
  %keep_flags = add nsw i32 %x, %y
  %sum = mul i32 %first, %duplicate
  %keep_freeze_a = freeze i32 poison
  %keep_freeze_b = freeze i32 poison
  %keep_load_a = load i32, ptr @data
  %keep_load_b = load i32, ptr @data
  br label %next
next:
  %keep_other_block = add i32 %x, %y
  ret i32 %sum
}

define i32 @main() {
  %a = call i32 @calculate(i32 2, i32 3)
  %ok = icmp eq i32 %a, 25
  %r = select i1 %ok, i32 0, i32 1
  ret i32 %r
}
