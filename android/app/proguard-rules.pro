# Native code (SDL and Luanti) looks up these classes, methods and fields
# by name through JNI, so R8 must not remove or rename them.
-keep class org.libsdl.app.** { *; }
-keep class net.minetest.minetest.** { *; }
-keepclasseswithmembernames,includedescriptorclasses class * {
	native <methods>;
}
