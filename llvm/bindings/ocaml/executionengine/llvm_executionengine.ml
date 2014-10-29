(*===-- llvm_executionengine.ml - LLVM OCaml Interface --------*- OCaml -*-===*
 *
 *                     The LLVM Compiler Infrastructure
 *
 * This file is distributed under the University of Illinois Open Source
 * License. See LICENSE.TXT for details.
 *
 *===----------------------------------------------------------------------===*)

exception Error of string

let () = Callback.register_exception "Llvm_executionengine.Error" (Error "")

module CodeModel = struct
  type t =
  | Default
  | JIT_default
  | Small
  | Kernel
  | Medium
  | Large
end

module GenericValue = struct
  type t

  external of_float: Llvm.lltype -> float -> t
    = "llvm_genericvalue_of_float"
  external of_pointer: 'a -> t
    = "llvm_genericvalue_of_pointer"
  external of_int32: Llvm.lltype -> int32 -> t
    = "llvm_genericvalue_of_int32"
  external of_int: Llvm.lltype -> int -> t
    = "llvm_genericvalue_of_int"
  external of_nativeint: Llvm.lltype -> nativeint -> t
    = "llvm_genericvalue_of_nativeint"
  external of_int64: Llvm.lltype -> int64 -> t
    = "llvm_genericvalue_of_int64"

  external as_float: Llvm.lltype -> t -> float
    = "llvm_genericvalue_as_float"
  external as_pointer: t -> 'a
    = "llvm_genericvalue_as_pointer"
  external as_int32: t -> int32
    = "llvm_genericvalue_as_int32"
  external as_int: t -> int
    = "llvm_genericvalue_as_int"
  external as_nativeint: t -> nativeint
    = "llvm_genericvalue_as_nativeint"
  external as_int64: t -> int64
    = "llvm_genericvalue_as_int64"
end


module ExecutionEngine = struct
  type t

  type compileroptions = {
    opt_level: int;
    code_model: CodeModel.t;
    no_framepointer_elim: bool;
    enable_fast_isel: bool;
  }

  let default_compiler_options = {
    opt_level = 0;
    code_model = CodeModel.JIT_default;
    no_framepointer_elim = false;
    enable_fast_isel = false }

  external create: Llvm.llmodule -> t
    = "llvm_ee_create"
  external create_interpreter: Llvm.llmodule -> t
    = "llvm_ee_create_interpreter"
  external create_jit: Llvm.llmodule -> int -> t
    = "llvm_ee_create_jit"
  external create_mcjit: Llvm.llmodule -> compileroptions -> t
    = "llvm_ee_create_mcjit"
  external dispose: t -> unit
    = "llvm_ee_dispose"
  external add_module: Llvm.llmodule -> t -> unit
    = "llvm_ee_add_module"
  external remove_module: Llvm.llmodule -> t -> Llvm.llmodule
    = "llvm_ee_remove_module"
  external find_function: string -> t -> Llvm.llvalue option
    = "llvm_ee_find_function"
  external run_function: Llvm.llvalue -> GenericValue.t array -> t ->
                         GenericValue.t
    = "llvm_ee_run_function"
  external run_static_ctors: t -> unit
    = "llvm_ee_run_static_ctors"
  external run_static_dtors: t -> unit
    = "llvm_ee_run_static_dtors"
  external run_function_as_main: Llvm.llvalue -> string array ->
                                 (string * string) array -> t -> int
    = "llvm_ee_run_function_as_main"
  external free_machine_code: Llvm.llvalue -> t -> unit
    = "llvm_ee_free_machine_code"

  external data_layout : t -> Llvm_target.DataLayout.t
    = "llvm_ee_get_data_layout"

  (* The following are not bound. Patches are welcome.

  add_global_mapping: llvalue -> llgenericvalue -> t -> unit
  clear_all_global_mappings: t -> unit
  update_global_mapping: llvalue -> llgenericvalue -> t -> unit
  get_pointer_to_global_if_available: llvalue -> t -> llgenericvalue
  get_pointer_to_global: llvalue -> t -> llgenericvalue
  get_pointer_to_function: llvalue -> t -> llgenericvalue
  get_pointer_to_function_or_stub: llvalue -> t -> llgenericvalue
  get_global_value_at_address: llgenericvalue -> t -> llvalue option
  store_value_to_memory: llgenericvalue -> llgenericvalue -> lltype -> unit
  initialize_memory: llvalue -> llgenericvalue -> t -> unit
  recompile_and_relink_function: llvalue -> t -> llgenericvalue
  get_or_emit_global_variable: llvalue -> t -> llgenericvalue
  disable_lazy_compilation: t -> unit
  lazy_compilation_enabled: t -> bool
  install_lazy_function_creator: (string -> llgenericvalue) -> t -> unit

   *)
end

external initialize_native_target : unit -> bool
                                  = "llvm_initialize_native_target"
