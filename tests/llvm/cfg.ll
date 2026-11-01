; LV-PASSES: lv-cfg
; LV-EXPECT: ret i32 7
; LV-EXPECT: address_taken:
; LV-REJECT: dead_block:
; LV-REJECT: dead_cycle_a:
; LV-REJECT: dead_cycle_b:
; LV-EXIT: 0

@address = global ptr blockaddress(@preserve_address, %address_taken)

define i32 @repair_phi() {
entry:
  br label %exit
dead_block:
  br label %exit
dead_cycle_a:
  br label %dead_cycle_b
dead_cycle_b:
  br label %dead_cycle_a
exit:
  %answer = phi i32 [ 7, %entry ], [ 13, %dead_block ]
  ret i32 %answer
}

define void @preserve_address() {
entry:
  ret void
address_taken:
  ret void
}

define i32 @main() {
  %a = call i32 @repair_phi()
  %ok = icmp eq i32 %a, 7
  %r = select i1 %ok, i32 0, i32 1
  ret i32 %r
}
