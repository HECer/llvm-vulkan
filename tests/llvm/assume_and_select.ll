; LV-PASSES: lv-pipeline
; LV-EXPECT: call void @llvm.assume(i1 %positive)
; LV-EXPECT: %positive = icmp sgt i32 %x, 0
; LV-EXPECT: %first_freeze = freeze i32 %x
; LV-EXPECT: %second_freeze = freeze i32 %x
; LV-EXPECT: %selected = select i1 true, i32 %x, i32 poison
; LV-EXPECT: call void @llvm.sideeffect()
; LV-REJECT: %dead_chain
; LV-EXIT: 0

declare void @llvm.assume(i1 noundef)
declare void @llvm.sideeffect()

define i32 @check(i32 noundef %x) {
  %positive = icmp sgt i32 %x, 0
  call void @llvm.assume(i1 %positive)
  %first_freeze = freeze i32 %x
  %second_freeze = freeze i32 %x
  %selected = select i1 true, i32 %x, i32 poison
  %dead_chain0 = mul i32 %x, 3
  %dead_chain1 = add i32 %dead_chain0, 5
  call void @llvm.sideeffect()
  %sum0 = add i32 %first_freeze, %second_freeze
  %sum1 = add i32 %sum0, %selected
  ret i32 %sum1
}

define i32 @main() {
  %a = call i32 @check(i32 7)
  %ok = icmp eq i32 %a, 21
  %r = select i1 %ok, i32 0, i32 1
  ret i32 %r
}
