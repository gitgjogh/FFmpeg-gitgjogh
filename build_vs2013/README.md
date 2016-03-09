DEBUG FFMPEG USING VS2013
=========================

You need at least VS2013 update 2 to compile x264.

## Build Libav*** Using MSVC Compiler Under MINGW32

### Import `vcvar` Into MSYS (MINGW32)

- **Scheme#1:**  From `Start Menu`->`vs2013`->`tools`->`VS2013 x86 本机工具命令提示` start `msys.bat` binding with `mingw32`. (Don't use `mingw64`) 
- **Scheme#2:**  Add `call "C:/Program Files/Microsoft Visual Studio 12.0/VC/vcvarsall.bat"` to the 1st line of `msys.bat`. Then kick off `msys.bat`. 

Confirm `vcvar` has been imported:  

    $ which link nmake  
    /c/Program Files (x86)/Microsoft Visual Studio 12.0/VC/BIN/link.exe  
    /c/Program Files (x86)/Microsoft Visual Studio 12.0/VC/BIN/nmake.exe  

`nmake` is the make programe under MSVC. Needed for making `zlib`. See later.

### Fix Collision Between `VC-link` And `MSYS-LINK`

- `VC-link` links obj/libs files to create exe/dll file
- `MSYS-LINK` creates a link named FILE2 to an existing FILE1.

It is possible that `msys's file linker` or `coreutils's linker` conflicts with `MSVC’s linker`. You can find out by running `which link` to see which `link` binary you are using. If it is located at `/bin/link.exe`, mv `/bin/link.exe` to `/bin/msys-link.exe`

### Yasm

### X264

Go to an `x264` source tree 

	> CC=cl $ ./configure --prefix='/c/Program Files (x86)/Microsoft Visual Studio 12.0/VC/' --enable-static --disable-gpl --enable-debug --enable-win32thread
	> $ make && make install

### Zlib

- `$ cd zlib-1.2.8`
- Edit `win32/Makefile.msc` so that `CFLAGS` uses `-MT` instead of `-MD`, since this is how FFmpeg is built as well.  
    > del CFLAGS  = -nologo -MD -W3 -O2 -Oy--Zi -Fd"zlib" $(LOC)  
    > add CFLAGS  = -nologo -MT -W3 -O2 -Oy -Zi -Fd"zlib" $(LOC)  
- Edit `zconf.h` and remove its inclusion of `unistd.h`. This gets erroneously included when building FFmpeg.   
- `$ nmake -f win32/Makefile.msc`
- Move `zlib.lib`, `zconf.h` and 	zlib.h	 to somewhere MSVC can see.   
	> $cp zlib.lib '/c/Program Files (x86)/Microsoft Visual Studio 12.0/VC/lib/'  
	> $cp zconf.h zlib.h '/c/Program Files (x86)/Microsoft Visual Studio 12.0/VC/include/'  

**reference:** <http://www.ffmpeg.org/platform.html> 

### Build FFmpeg Libs Using `msvc` Toolchain 

under `build_vs2013`:  

	> $ madir build && cd build  
    > $ ../../configure --prefix=`pwd`/.. --toolchain=msvc --disable-shared --enable-static --enable-libx264 --enable-gpl --extra-cflags=-I/mingw/include --extra-ldflags=-L/mingw/lib 
    > $ make --debug | tee make.log  
    > $ make install  


## Create Win32 Console App. Project

Add the following files to the project:  

		cmdutils.h cmdutils_common_opt.h build/config.h ffmpeg.h  
		cmdutils.c ffmpeg.c ffmpeg_dxva2.cffmpeg_filter.c ffmpeg_opt.c  

**Clue:** Run `grep "^CC" make.log` to see what files are compiled.

### Add Dependency

- Additional Include Directories: `include;build`  
- Additional Library Directories: `lib`  
- Additional Dependent Libraries:   `vfw32.lib;user32.lib;gdi32.lib;psapi.lib;ole32.lib;strmiids.lib;uuid.lib;oleaut32.lib;shlwapi.lib;advapi32.lib;shell32.lib;libavdevice.a;libavfilter.a;libswscale.a;libavformat.a;libavcodec.a;libswresample.a;libavutil.a;`    

**Clue#1:** Under `build_vs2013`, run `env PKG_CONFIG_PATH=lib/pkgconfig/ pkg-config --libs libavdevice` to see what additional libs need to be added. For example, my result is:  

		vfw32.lib user32.lib gdi32.lib psapi.lib ole32.lib strmiids.lib uuid.lib oleaut32.lib shlwapi.lib ws2_32.lib advapi32.lib shell32.lib -Ld:/repo/ffmpeg_vs2013/ffmpeg_dev/lib -lavdevice -lavfilter -lswscale -lavresample -lavformat -lavcodec -lswresample -lavutil

**Clue#2:** The following lines in the generated 'Malefile' also can be took as reference to decide lib orders  

		# $(FFLIBS-yes) needs to be in linking order
		FFLIBS-$(CONFIG_AVDEVICE)   += avdevice
		FFLIBS-$(CONFIG_AVFILTER)   += avfilter
		FFLIBS-$(CONFIG_AVFORMAT)   += avformat
		FFLIBS-$(CONFIG_AVCODEC)    += avcodec
		FFLIBS-$(CONFIG_AVRESAMPLE) += avresample
		FFLIBS-$(CONFIG_POSTPROC)   += postproc
		FFLIBS-$(CONFIG_SWRESAMPLE) += swresample
		FFLIBS-$(CONFIG_SWSCALE)    += swscale

### Set Preprocessor #

	-D_ISOC99_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -Dstrtod=avpriv_strtod -Dsnprintf=avpriv_snprintf -D_snprintf=avpriv_snprintf -Dvsnprintf=avpriv_vsnprintf -D_WIN32_WINNT=0x0502 -nologo -D_USE_MATH_DEFINES -D_CRT_SECURE_NO_WARNINGS -Dinline=__inline -FIstdlib.h -Dstrtoll=_strtoi64

**Clue#1:** search 'CPPFLAGS','CFLAGS','CXXFLAGS'.'ASFLAGS' in 'config.mak' to see how autoconfig deal with 'inline','snprintf' compatibility by setting '--toolchain=msvc' option.

**Clue#2:** search '^cl' in 'config.log', see that how MSVC compiler 'cl.exe' is called. Here is an example:

	"cl -D_ISOC99_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -Dstrtod=avpriv_strtod -Dsnprintf=avpriv_snprintf -D_snprintf=avpriv_snprintf -Dvsnprintf=avpriv_vsnprintf -D_WIN32_WINNT=0x0502 -nologo -D_USE_MATH_DEFINES -D_CRT_SECURE_NO_WARNINGS -Dinline=__inline -FIstdlib.h -Dstrtoll=_strtoi64 -c -Fo./ffconf.bVoQLIYP.o ./ffconf.AiyPZYQi.c"

## Problems

### Fix Collision Between `LIBCMT.lib` and `msvcrtd.lib`

	1>------ 已启动生成:  项目: build_vs2013, 配置: Debug Win32 ------  
	1>LIBCMT.lib(open.obj) : error LNK2005: __sopen 已经在 MSVCRT.lib(MSVCR120.dll) 中定义  
	1>LIBCMT.lib(invarg.obj) : error LNK2005: __invoke_watson 已经在 MSVCRT.lib(MSVCR120.dll) 中定义  
	1>LIBCMT.lib(_file.obj) : error LNK2005: ___iob_func 已经在 MSVCRT.lib(MSVCR120.dll) 中定义  

**reason:** some code link to the static RTL(run time library), while some code link to dynamic RTL
solution: set msvcr to /MT in "properties->C/C++->Code generation"  

**reference:** <https://msdn.microsoft.com/en-us/library/2kzt1wy3%28v=VS.71%29.aspx>

	
### Undefined Reference To 'avresample_version', 'postproc_version'

two scheme:  

	1) goto "cmdutil.c"::"print_all_libs_info()", comment the following two lines:
	    // PRINT_LIB_INFO(avresample, AVRESAMPLE, flags, level);
	    // PRINT_LIB_INFO(postproc, POSTPROC, flags, level);
	2) configure --enable-avresample

### Network Module

which has to link to the WINDOWS SDK, so be careful with how '_WIN32_WINNT' defined in 'config.mak' or 'config.log', copied it to the VC preprocessors. Reference to the "set preprocessor" section
