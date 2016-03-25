; RUN: llc -mtriple thumbv7--windows-itanium -print-machineinstrs=expand-isel-pseudos -o /dev/null %s 2>&1 | FileCheck %s -check-prefix CHECK-DIV

; int f(int n, int d) {
;   if (n / d)
;     return 1;
;   return 0;
; }

define arm_aapcs_vfpcc i32 @f(i32 %n, i32 %d) {
entry:
  %retval = alloca i32, align 4
  %n.addr = alloca i32, align 4
  %d.addr = alloca i32, align 4
  store i32 %n, i32* %n.addr, align 4
  store i32 %d, i32* %d.addr, align 4
  %0 = load i32, i32* %n.addr, align 4
  %1 = load i32, i32* %d.addr, align 4
  %div = sdiv i32 %0, %1
  %tobool = icmp ne i32 %div, 0
  br i1 %tobool, label %if.then, label %if.end

if.then:
  store i32 1, i32* %retval, align 4
  br label %return

if.end:
  store i32 0, i32* %retval, align 4
  br label %return

return:
  %2 = load i32, i32* %retval, align 4
  ret i32 %2
}

; CHECK-DIV-DAG: BB#0
; CHECK-DIV-DAG: Successors according to CFG: BB#5({{.*}}) BB#4
; CHECK-DIV-DAG: BB#1
; CHECK-DIV-DAG: Successors according to CFG: BB#3
; CHECK-DIV-DAG: BB#2
; CHECK-DIV-DAG: Successors according to CFG: BB#3
; CHECK-DIV-DAG: BB#3
; CHECK-DIV-DAG: BB#4
; CHECK-DIV-DAG: Successors according to CFG: BB#1({{.*}}) BB#2
; CHECK-DIV-DAG: BB#5

; RUN: llc -mtriple thumbv7--windows-itanium -print-machineinstrs=expand-isel-pseudos -o /dev/null %s 2>&1 | FileCheck %s -check-prefix CHECK-MOD

; int r;
; int g(int l, int m) {
;   if (m <= 0)
;     return 0;
;   return (r = l % m);
; }

@r = common global i32 0, align 4

define arm_aapcs_vfpcc i32 @g(i32 %l, i32 %m) {
entry:
  %cmp = icmp eq i32 %m, 0
  br i1 %cmp, label %return, label %if.end

if.end:
  %rem = urem i32 %l, %m
  store i32 %rem, i32* @r, align 4
  br label %return

return:
  %retval.0 = phi i32 [ %rem, %if.end ], [ 0, %entry ]
  ret i32 %retval.0
}

; CHECK-MOD-DAG: BB#0
; CHECK-MOD-DAG: Successors according to CFG: BB#2({{.*}}) BB#1
; CHECK-MOD-DAG: BB#1
; CHECK-MOD-DAG: Successors according to CFG: BB#4({{.*}}) BB#3
; CHECK-MOD-DAG: BB#2
; CHECK-MOD-DAG: BB#3
; CHECK-MOD-DAG: Successors according to CFG: BB#2
; CHECK-MOD-DAG: BB#4

; RUN: llc -mtriple thumbv7--windows-itanium -print-machineinstrs=expand-isel-pseudos -filetype asm -o - %s 2>&1 | FileCheck %s -check-prefix CHECK-CFG

; unsigned c;
; extern unsigned long g(void);
; int f(unsigned u, signed char b) {
;   if (b)
;     c = g() % u;
;   return c;
; }

@c = common global i32 0, align 4

declare arm_aapcs_vfpcc i32 @i()

define arm_aapcs_vfpcc i32 @h(i32 %u, i8 signext %b) #0 {
entry:
  %tobool = icmp eq i8 %b, 0
  br i1 %tobool, label %entry.if.end_crit_edge, label %if.then

entry.if.end_crit_edge:
  %.pre = load i32, i32* @c, align 4
  br label %if.end

if.then:
  %call = tail call arm_aapcs_vfpcc i32 @i()
  %rem = urem i32 %call, %u
  store i32 %rem, i32* @c, align 4
  br label %if.end

if.end:
  %0 = phi i32 [ %.pre, %entry.if.end_crit_edge ], [ %rem, %if.then ]
  ret i32 %0
}

attributes #0 = { optsize }

; CHECK-CFG-DAG: BB#0
; CHECK-CFG_DAG: t2Bcc <BB#2>
; CHECK-CFG-DAG: t2B <BB#1>

; CHECK-CFG-DAG: BB#1
; CHECK-CFG-DAG: t2B <BB#3>

; CHECK-CFG-DAG: BB#2
; CHECK-CFG-DAG: tCBZ %vreg{{[0-9]}}, <BB#5>
; CHECK-CFG-DAG: t2B <BB#4>

; CHECK-CFG-DAG: BB#4

; CHECK-CFG-DAG: BB#3
; CHECK-CFG-DAG: tBX_RET

; CHECK-CFG-DAG: BB#5
; CHECK-CFG-DAG: t2UDF 249

; CHECK-CFG-LABEL: h:
; CHECK-CFG: cbz r{{[0-9]}}, .LBB2_2
; CHECK-CFG: b .LBB2_4
; CHECK-CFG-LABEL: .LBB2_2:
; CHECK-CFG-NEXT: udf.w #249
; CHECK-CFG-LABEL: .LBB2_4:
; CHECK-CFG: bl __rt_udiv
; CHECK-CFG: pop.w {{{.*}}, r11, pc}

