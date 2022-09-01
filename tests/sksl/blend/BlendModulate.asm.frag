OpCapability Shader
%1 = OpExtInstImport "GLSL.std.450"
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %sk_Clockwise %sk_FragColor
OpExecutionMode %main OriginUpperLeft
OpName %sk_Clockwise "sk_Clockwise"
OpName %sk_FragColor "sk_FragColor"
OpName %_UniformBuffer "_UniformBuffer"
OpMemberName %_UniformBuffer 0 "src"
OpMemberName %_UniformBuffer 1 "dst"
OpName %blend_modulate_h4h4h4 "blend_modulate_h4h4h4"
OpName %main "main"
OpDecorate %sk_Clockwise BuiltIn FrontFacing
OpDecorate %sk_FragColor RelaxedPrecision
OpDecorate %sk_FragColor Location 0
OpDecorate %sk_FragColor Index 0
OpMemberDecorate %_UniformBuffer 0 Offset 0
OpMemberDecorate %_UniformBuffer 0 RelaxedPrecision
OpMemberDecorate %_UniformBuffer 1 Offset 16
OpMemberDecorate %_UniformBuffer 1 RelaxedPrecision
OpDecorate %_UniformBuffer Block
OpDecorate %11 Binding 0
OpDecorate %11 DescriptorSet 0
OpDecorate %19 RelaxedPrecision
OpDecorate %20 RelaxedPrecision
OpDecorate %21 RelaxedPrecision
OpDecorate %29 RelaxedPrecision
OpDecorate %33 RelaxedPrecision
%bool = OpTypeBool
%_ptr_Input_bool = OpTypePointer Input %bool
%sk_Clockwise = OpVariable %_ptr_Input_bool Input
%float = OpTypeFloat 32
%v4float = OpTypeVector %float 4
%_ptr_Output_v4float = OpTypePointer Output %v4float
%sk_FragColor = OpVariable %_ptr_Output_v4float Output
%_UniformBuffer = OpTypeStruct %v4float %v4float
%_ptr_Uniform__UniformBuffer = OpTypePointer Uniform %_UniformBuffer
%11 = OpVariable %_ptr_Uniform__UniformBuffer Uniform
%_ptr_Function_v4float = OpTypePointer Function %v4float
%15 = OpTypeFunction %v4float %_ptr_Function_v4float %_ptr_Function_v4float
%void = OpTypeVoid
%23 = OpTypeFunction %void
%_ptr_Uniform_v4float = OpTypePointer Uniform %v4float
%int = OpTypeInt 32 1
%int_0 = OpConstant %int 0
%int_1 = OpConstant %int 1
%blend_modulate_h4h4h4 = OpFunction %v4float None %15
%16 = OpFunctionParameter %_ptr_Function_v4float
%17 = OpFunctionParameter %_ptr_Function_v4float
%18 = OpLabel
%19 = OpLoad %v4float %16
%20 = OpLoad %v4float %17
%21 = OpFMul %v4float %19 %20
OpReturnValue %21
OpFunctionEnd
%main = OpFunction %void None %23
%24 = OpLabel
%30 = OpVariable %_ptr_Function_v4float Function
%34 = OpVariable %_ptr_Function_v4float Function
%25 = OpAccessChain %_ptr_Uniform_v4float %11 %int_0
%29 = OpLoad %v4float %25
OpStore %30 %29
%31 = OpAccessChain %_ptr_Uniform_v4float %11 %int_1
%33 = OpLoad %v4float %31
OpStore %34 %33
%35 = OpFunctionCall %v4float %blend_modulate_h4h4h4 %30 %34
OpStore %sk_FragColor %35
OpReturn
OpFunctionEnd
