; LV-PASSES: lv-pipeline
; LV-EXPECT: %keep_constant = add i32 20, 22
; LV-EXPECT: %keep_unused = mul i32 %x, 5
; LV-EXPECT: unreachable_block:

define i32 @untouched(i32 %x) noinline optnone {
entry:
  %keep_constant = add i32 20, 22
  %keep_unused = mul i32 %x, 5
  ret i32 %keep_constant
unreachable_block:
  ret i32 1
}
