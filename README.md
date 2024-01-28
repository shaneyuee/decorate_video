
##Installing dependencies

```
sudo yum install glew-devel freetype-devel mesa-libGL-devel mesa-libGLU-devel mesa-libEGL-devel glm-devel

sudo yum install -y libdc1394-devel OpenEXR OpenEXR-devel openjpeg2-devel wavpack-devel fontconfig-devel harfbuzz-devel fribidi-devel
```

Webp

https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.3.2-linux-x86-64.tar.gz


ffmpeg

https://www.johnvansickle.com/ffmpeg/old-releases/ffmpeg-5.1.1-i686-static.tar.xz


##Run example

```
./decorateVideo --disable_opengl --blind_watermark=text:TEST:5 out.mp4:1080:1920 mainvideo:1:intro.mp4:0:0:1080:1920:100:0:0
```
