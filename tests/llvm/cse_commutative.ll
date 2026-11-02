; LV-PASSES: lv-cse
; LV-EXPECT: %first = add i32 %x, %y
; LV-EXPECT: %sum = mul i32 %first, %first
; LV-EXPECT: %and_a = and i32 %x, %y
; LV-EXPECT: %and_prod = mul i32 %and_a, %and_a
; LV-REJECT: %comm_add =
; LV-REJECT: %comm_and =
; LV-EXIT: 0

define i32 @test_commutative(i32 %x, i32 %y) {
entry:
  %first = add i32 %x, %y
  %comm_add = add i32 %y, %x
  %sum = mul i32 %first, %comm_add
  %and_a = and i32 %x, %y
  %comm_and = and i32 %y, %x
  %and_prod = mul i32 %and_a, %comm_and
  %res = add i32 %sum, %and_prod
  ret i32 %res
}

define i32 @main() {
  %a = call i32 @test_commutative(i32 3, i32 5)
  ; (%first = 8, %comm_add = 8 -> %sum = 64)
  ; (%and_a = 1, %comm_and = 1 -> %and_prod = 1)
  ; %res = 65
  %ok = icmp eq i32 %a, 65
  %r = select i1 %ok, i32 0, i32 1
  ret i32 %r
}
