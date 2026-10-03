const char *fallbackShader_greyscale_vp =
"attribute vec4 attr_Position;\n"
"attribute vec2 attr_TexCoord0;\n"
"varying   vec2 var_TexCoords;\n"
"\n"
"void main() {\n"
"    vec2 clipXY = (attr_Position.xy * r_FBufScale) * 2.0 - 1.0;\n"
"    clipXY.y = -clipXY.y;\n"
"\n"
"    gl_Position   = vec4(clipXY, 0.0, 1.0);\n"
"    var_TexCoords = attr_TexCoord0;\n"
"}\n"
;
