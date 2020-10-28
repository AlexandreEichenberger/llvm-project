// RUN: mlir-opt -std-to-std-lowering %s -split-input-file | FileCheck %s

// Test floor divide with signed integer
// CHECK-LABEL:       func @floordivi
// CHECK-SAME:     ([[VAR_arg0:%.+]]: i32, [[VAR_arg1:%.+]]: i32) -> i32 {
func @floordivi(%arg0: i32, %arg1: i32) -> (i32) {
  %res = floordivi_signed %arg0, %arg1 : i32
  return %res : i32
// CHECK:           [[ONE:%.+]] = constant 1 : i32
// CHECK:           [[ZERO:%.+]] = constant 0 : i32
// CHECK:           [[MIN1:%.+]] = constant -1 : i32
// CHECK:           [[CMP1:%.+]] = cmpi "slt", [[VAR_arg1]], [[ZERO]] : i32
// CHECK:           [[X:%.+]] = select [[CMP1]], [[ONE]], [[MIN1]] : i32
// CHECK:           [[TRUE1:%.+]] = subi [[X]], [[VAR_arg0]] : i32
// CHECK:           [[TRUE2:%.+]] = divi_signed [[TRUE1]], [[VAR_arg1]] : i32
// CHECK:           [[TRUE3:%.+]] = subi [[MIN1]], [[TRUE2]] : i32
// CHECK:           [[FALSE:%.+]] = divi_signed [[VAR_arg0]], [[VAR_arg1]] : i32
// CHECK:           [[VAL:%.+]] = muli [[VAR_arg0]], [[VAR_arg1]] : i32
// CHECK:           [[CMP2:%.+]] = cmpi "slt", [[VAL]], [[ZERO]] : i32
// CHECK:           [[RES:%.+]] = select [[CMP2]], [[TRUE3]], [[FALSE]] : i32
// CHECK:           return [[RES]] : i32
// CHECK:         }
}

// -----

// Test ceil divide with signed integer
// CHECK-LABEL:       func @ceildivi
// CHECK-SAME:     ([[ARG0:%.+]]: i32, [[ARG1:%.+]]: i32) -> i32 {
func @ceildivi(%arg0: i32, %arg1: i32) -> (i32) {
  %res = ceildivi_signed %arg0, %arg1 : i32
  return %res : i32

// CHECK:           [[ONE:%.+]] = constant 1 : i32
// CHECK:           [[ZERO:%.+]] = constant 0 : i32
// CHECK:           [[MINONE:%.+]] = constant -1 : i32
// CHECK:           [[CMP1:%.+]] = cmpi "sgt", [[ARG1]], [[ZERO]] : i32
// CHECK:           [[X:%.+]] = select [[CMP1]], [[MINONE]], [[ONE]] : i32
// CHECK:           [[TRUE1:%.+]] = addi [[X]], [[ARG0]] : i32
// CHECK:           [[TRUE2:%.+]] = divi_signed [[TRUE1]], [[ARG1]] : i32
// CHECK:           [[TRUE3:%.+]] = addi [[ONE]], [[TRUE2]] : i32
// CHECK:           [[FALSE1:%.+]] = subi [[ZERO]], [[ARG0]] : i32
// CHECK:           [[FALSE2:%.+]] = divi_signed [[FALSE1]], [[ARG1]] : i32
// CHECK:           [[FALSE3:%.+]] = subi [[ZERO]], [[FALSE2]] : i32
// CHECK:           [[VAL:%.+]] = muli [[ARG0]], [[ARG1]] : i32
// CHECK:           [[CMP2:%.+]] = cmpi "sgt", [[VAL]], [[ZERO]] : i32
// CHECK:           [[RES:%.+]] = select [[CMP2]], [[TRUE3]], [[FALSE3]] : i32
// CHECK:           return [[RES]] : i32
// CHECK:         }
// CHECK:       }
}
