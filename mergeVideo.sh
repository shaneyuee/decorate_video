#!/bin/bash

VERSION=V0.0.1_alpha5_20230706
PROG=$(basename $0)
echo "$PROG version $VERSION"
echo "Merge video tool with transition effects if needed."

if (($# < 3));
then
	echo "Error: bad arguents, usage:";
	echo "$PROG <options> <OutVideo> <InputVideo1>:<Effect1> <InputVideo2>:<Effect2> ... ";
	echo "Available options:";
	echo "  --use_effect=<effect>    #set global effect, set to 'no' to disable effect, default is 'fade'";
	echo "  --cross_time=<duration>  #e.g. 2.5s, 1500ms";
	echo "  --use_dynamic=[yes|no]   #yes-apply while playing video, no-apply to last/first images";
	echo "  --log_level=[error|info|debug] #set log level, default is info";
	echo "Available effects:";
	echo "  fade";
	echo "  radial";
	echo "  rectcrop";
	echo "  see more at https://trac.ffmpeg.org/wiki/Xfade";
	exit 1;
fi

log_level=info

function get_time_ms()
{
	# Mac OS does not support %N, replace with 000
	date +%s%3N | sed s/'3N'/'000'/;
}

function run_ffmpeg()
{
	ffmpeg_level="-v error"
	if [ "$log_level" == "debug" ]; then
		ffmpeg_level=
	fi
	if [ "$log_level" != "error" ]; then
		echo "Running: ffmpeg $@" >&2;
	fi
	if [ -f /var/local/decorate_video/ffmpeg ];
	then
		/var/local/decorate_video/ffmpeg $ffmpeg_level "$@"
	else
		ffmpeg $ffmpeg_level "$@"
	fi
}

function run_ffprobe()
{
	if [ "$log_level" != "error" ]; then
		echo "Running: ffprobe $@" >&2;
	fi
	if [ -f /var/local/decorate_video/ffprobe ];
	then
		/var/local/decorate_video/ffprobe "$@"
	else
		ffprobe "$@"
	fi
}

function get_video_duration()
{
	d=$(run_ffprobe -v error -select_streams v:0 -show_entries stream=duration -of default=nokey=1:noprint_wrappers=1 "$1")
	if [ -z "$d" ]; then
		echo "Error: failed to get duration for video $1" >&2;
		return 1;
	fi
	echo "scale=3; $d/1"|bc -l
	return 0;
}

function get_audio_fmt()
{
	i=$(run_ffprobe -v error -select_streams a:0 -show_entries stream=codec_name,channel_layout,sample_fmt,sample_rate -of default=nokey=0:noprint_wrappers=1 "$1")
	if [ -z "$i" ]; then
		echo "Error: failed to get audio info for $1" >&2;
		return 1;
	fi
	echo $i
	return 0;
}

function get_video_fmt()
{
	i=$(run_ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,codec_tag_string,r_frame_rate,width,height,pix_fmt,time_base -of default=nokey=0:noprint_wrappers=1 "$1")
	if [ -z "$i" ]; then
		echo "Error: failed to get audio info for $1" >&2;
		return 1;
	fi
	echo $i
	return 0;
}

effect="fade"
duration=2
dynamic=0
output=
inputs=
first_video_fmt=
first_audio_fmt=
is_video_same_fmt=1
is_audio_same_fmt=1
start_time=$(get_time_ms)

function add_tmpfile()
{
	echo "$@" >> $output.tmpfiles;
}

function remove_tmpfiles()
{
	tmpfiles=$(cat $output.tmpfiles)
	if ! [ -z "$tmpfiles" ];
	then
		if [ "$log_level" == "debug" ];
		then
			echo "Warning: the temporary files are not deleted automatically in debug mode, you should delete them manually.";
			echo "Tmp files:[$tmpfiles]";
		else
			echo "Removing tmp files:[$tmpfiles]";
			rm -f $(echo $tmpfiles|xargs)
		fi
		rm -f $output.tmpfiles
	fi
}

for a in "$@";
do
	if (echo $a|grep -i "^--use_effect=" >/dev/null 2>&1);
	then
		effect=$(echo $a|sed s/'--use_effect='// 2>/dev/null)
		if [ "$effect" == "no" ] || [ "$effect" == "NO" ];
		then
			effect=
		fi
		continue;
	fi
	if (echo $a|grep -i "^--cross_time=" >/dev/null 2>&1);
	then
		duration=$(echo $a|sed s/'--cross_time='// 2>/dev/null);

		if (echo $duration |grep -E "^[0-9.]*s$"  >/dev/null 2>&1);
		then
			duration=$(echo $duration|sed s/'s'// 2>/dev/null);
			continue;
		fi
		if (echo $duration |grep -E "^[0-9]*ms$"  >/dev/null 2>&1);
		then
			duration=$(echo $duration|sed s/'ms'// 2>/dev/null);
			duration=$(echo "scale=3; $duration/1000"|bc -l|sed s/'^\.'/'0.'/)
			continue;
		fi
		echo "Error: unrecognized cross time format: $duration" >&2;
		remove_tmpfiles;
		exit 1;
	fi
	if (echo $a|grep -i "^--log_level=" >/dev/null 2>&1);
	then
		log_level=$(echo $a|sed s/'--log_level='// 2>/dev/null)
		if [ "$log_level" == "err" ]; then log_level=error; fi
		if [ "$log_level" == "inf" ]; then log_level=info; fi
		if [ "$log_level" == "dbg" ]; then log_level=debug; fi
		if [ "$log_level" != "error" ] && [ "$log_level" != "info" ] && [ "$log_level" != "debug" ];
		then
			echo "Error: unrecognized log_level: $log_level" >&2;
			remove_tmpfiles;
			exit 1;
		fi
		continue;
	fi

	if (echo $a|grep -i "^--use_dynamic=yes" >/dev/null 2>&1);
	then
		dynamic=1
		continue;
	fi
	if (echo $a|grep -i "^--use_dynamic=no" >/dev/null 2>&1);
	then
		dynamic=0
		continue;
	fi

	if (echo $a|grep -i "^--" >/dev/null 2>&1);
	then
		remove_tmpfiles;
		exit 1;
	fi

	if [ -z "$output" ];
	then
		output="$a";
		rm -f $output.tmpfiles
		continue;
	fi

	v=$(echo $a|cut -d: -f1) # video file
	e=$(echo "$a"|grep ':'|cut -d: -f2) # effect

	video_fmt=$(get_video_fmt "$v");
	audio_fmt=$(get_audio_fmt "$v");
	if [ -z "$video_fmt" ] || [ -z "$audio_fmt" ];
	then
		if [ -z "$video_fmt" ]; then
			echo "Error: failed to get video format for $a" >&2;
		else
			echo "Error: failed to get audio format for $a" >&2;
		fi
		remove_tmpfiles;
		exit 1;
	fi

	cl=$(echo $audio_fmt|sed s/' '/'\n'/g|grep 'channel_layout'|cut -d= -f2)
	sr=$(echo $audio_fmt|sed s/' '/'\n'/g|grep 'sample_rate'|cut -d= -f2)
	ac=$(echo $audio_fmt|sed s/' '/'\n'/g|grep 'codec_name'|cut -d= -f2)
	sf=$(echo $audio_fmt|sed s/' '/'\n'/g|grep 'sample_fmt'|cut -d= -f2)
	fps=$(echo $video_fmt|sed s/' '/'\n'/g|grep 'frame_rate'|cut -d= -f2)
	pix=$(echo $video_fmt|sed s/' '/'\n'/g|grep 'pix_fmt'|cut -d= -f2)
	vc=$(echo $video_fmt|sed s/' '/'\n'/g|grep 'codec_name'|cut -d= -f2)
	vtag=$(echo $video_fmt|sed s/' '/'\n'/g|grep 'codec_tag_string'|cut -d= -f2)
	tb=$(echo $video_fmt|sed s/' '/'\n'/g|grep 'time_base'|cut -d= -f2|cut -d/ -f2)
	w=$(echo $video_fmt|sed s/' '/'\n'/g|grep 'width'|cut -d= -f2|cut -d/ -f2)
	h=$(echo $video_fmt|sed s/' '/'\n'/g|grep 'height'|cut -d= -f2|cut -d/ -f2)
	if [ -z "$first_w" ]; then first_w=$w; fi
	if [ -z "$first_h" ]; then first_h=$h; fi
	# if video format is not standard h264/yuv420p, than reformat it
	if [ "$vc" != "h264" ] || [ "$vtag" != "avc1" ] || [ "$pix" != "yuv420p" ] || [ $first_w -ne $w ] || [ $first_h -ne $h ];
	then
		echo "Warning: first video is not standard h264/yuv420p video, convert it." >&2;
		if ! (run_ffmpeg -i $v -pix_fmt yuv420p -c:v libx264 -framerate 25 -c:a aac -ar 16000 -ac 1 \
							-s ${first_w}x${first_h} -b:v 8000000 -video_track_timescale 12800 -y $v.std.mp4);
		then
			echo "Error: failed to convert video $v to standard format." >&2
			remove_tmpfiles;
			exit 1;
		fi
		cl="mono";
		sr=16000;
		ac="aac";
		sf="fltp";
		fps=25;
		pix="yuv420p";
		vc="h264";
		vtag="avc1";
		tb=12800;
		w=$first_w;
		h=$first_h;
		if [ -z "$e" ];
		then
			a=$v.std.mp4;
		else
			a="$v.std.mp4:$e";
		fi
		add_tmpfile "$v.std.mp4";
	fi

	if [ -z "$first_video_fmt" ];
	then
		first_video_fmt="$video_fmt";
		first_audio_fmt="$audio_fmt";
		first_cl=$cl
		first_sr=$sr
		first_ac=$ac
		first_sf=$sf
		first_fps=$fps
		first_pix=$pix
		first_vc=$vc
		first_vtag=$vtag
		first_tb=$tb
		first_w=$w
		first_h=$h
	else # convert to same format as first video
		if [ "$first_video_fmt" != "$video_fmt" ] || [ "$first_audio_fmt" != "$audio_fmt" ];
		then
			c="-ac 2"
			if [ "$first_cl" == "mono" ];
			then
				c="-ac 1"
			fi
			if ! (run_ffmpeg -i $v -framerate $first_fps -c:v copy -c:a $first_ac \
			           -video_track_timescale $first_tb -ar $first_sr $c -y $v.std.mp4);
			then
				echo "Error: failed to convert video $v to standard format." >&2
				remove_tmpfiles;
				exit 1
			fi
			if [ -z "$e" ];
			then
				a=$v.std.mp4;
			else
				a="$v.std.mp4:$e";
			fi
			add_tmpfile "$v.std.mp4";
		fi
	fi

	if [ -z "$inputs" ];
	then
		inputs=$(echo -ne "$a");
	else
		inputs=$(echo -ne "$inputs\n$a")
	fi
done

is_video_same_fmt=1
is_audio_same_fmt=1

if [ "$log_level" != "error" ]; then
	echo effect=$effect
	echo duration=$duration
	echo dynamic=$dynamic
	echo output=$output
	echo inputs=[$inputs] | xargs
	echo audiofmt=[$first_audio_fmt] | xargs
	echo videofmt=[$first_video_fmt] | xargs
fi

function gen_cross_video_static()
{
	prev=
	prev_vfa1=
	idx=0
	silent_audio=$output.silent.mp4

	#ffmpeg -f lavfi -i anullsrc=r=16000:cl=mono -t $duration -y out.mp3
	if ! (run_ffmpeg -f lavfi -i anullsrc=r=$first_sr:cl=$first_cl -t $duration -acodec $first_ac -y $silent_audio >&2);
	then
		echo "Error: failed to generate silent audio." >&2;
		return 1;
	fi
	add_tmpfile "$silent_audio";

	for f in `echo $inputs`;
	do
		a=$(echo "$f"|cut -d: -f1)
		e=$(echo "$f"|grep ':'|cut -d: -f2)
		if [ -z "$e" ]; then
			e=$effect
		fi
		duration_a=$(get_video_duration $a)
		if [ -z "$duration_a" ];
		then
			return 1;
		fi

		vfa0=${a}_first.bmp
		vfa1=${a}_last.bmp
		rm -f $vfa0 $vfa1
		run_ffmpeg -i $a -vframes 1 -f image2 $vfa0 >&2
		run_ffmpeg -ss $(echo "$duration_a-0.04"|bc) -i $a -vframes 1 -f image2 $vfa1 >&2
		if ! [ -f $vfa1 ]; then
			run_ffmpeg -ss $(echo "$duration_a-0.8"|bc) -i $a -vframes 1 -f image2 $vfa1 >&2
		fi
		if ! [ -f $vfa0 ] || ! [ -f $vfa1 ]; then
			echo "Error: failed to capture image for video $a" >&2;
			return 1
		fi
		add_tmpfile "$vfa0" "$vfa1";

		if [ -z "$prev_vfa1" ];
		then
			echo $a;
		else
			# generate cross video for prev_vfa1 -> vfa0
			v="$output.cross$idx.mp4";
			if ! (run_ffmpeg -loop 1 -framerate $first_fps -t $duration -i $prev_vfa1 \
			       -loop 1 -framerate $first_fps -t $duration -i $vfa0 \
				   -i $silent_audio \
			       -filter_complex "[0][1]xfade=transition=$e:offset=0:duration=$duration" \
			       -pix_fmt $first_pix -c:v libx264 -video_track_timescale $first_tb -c:a $first_ac -y $v >&2);
			then
				echo "Error: failed to generate cross video." >&2;
				return 1;
			fi
			add_tmpfile "$v";
			echo $v;
			echo $a;
		fi

		prev_vfa1=$vfa1;
		idx=$((idx+1));
	done

	return 0
}


# arguments a,b generate a_b_effect
function gen_cross_video_dynamic()
{
	idx=0
	ins=
	veffects=
	aeffects=
	prev=
	prev_ve=0:v
	prev_ae=0:a
	astart=0.0
	for f in `echo $inputs`;
	do
		a=$(echo "$f"|cut -d: -f1)
		e=$(echo "$f"|grep ':'|cut -d: -f2)
		if [ -z "$e" ]; then
			e=$effect
		fi
		duration_a=$(get_video_duration $a)
		if [ -z "$duration_a" ];
		then
			return 1;
		fi
		ve=$idx:v
		ae=$idx:a
		bstart=$astart;
		ins=$(echo "$ins -i $a");
		if ! [ -z "$prev" ];
		then
			ve="v$((idx-1))_$idx";
			ae="a$((idx-1))_$idx";
			bstart=$(echo "$astart+$duration_a-$duration"|bc)
			bstartms=$(echo "$bstart*1000"|bc)
			veffects=$(echo "$veffects[$prev_ve][$idx:v]xfade=transition=$e:offset=$bstart:duration=$duration[$ve];");
			aeffects=$(echo "$aeffects[$prev_ae]afade=t=out:st=$bstart:d=$duration:curve=tri[${prev_ae}_a1];[$idx:a]afade=t=in:st=0:d=$duration:curve=tri[${ae}_1];[${ae}_1]adelay=$bstartms|$bstartms[${ae}_2];[${prev_ae}_a1][${ae}_2]amix=inputs=2[$ae];");
		fi
		prev=$a;
		prev_ve=$ve;
		prev_ae=$ae;
		astart=$bstart;
		idx=$((idx+1));
	done

	effect_ab="$output.effect.mp4"
	c="-ac 2"
	if [ "$first_cl" == "mono" ];
	then
		c="-ac 1"
	fi
	if ! (run_ffmpeg `echo $ins` -filter_complex "${veffects}${aeffects}" -map "[$prev_ve]" -map "[$prev_ae]" \
	           -framerate $first_fps -pix_fmt $first_pix -c:v libx264 -video_track_timescale $first_tb $c -c:a $first_ac -y "$effect_ab" >&2);
	then
		echo "Error: failed to generate effect video $effect_ab." >&2
		return 1;
	fi
	add_tmpfile "$effect_ab";

	echo $effect_ab;

	return 0
}

# returns:
#  0 - succeed
#  1 - error
#  2 - no need of splitting, or missing key frame
function split_video()
{
	video_file=$1
	video_duration=$2
	split_time=$3
	min_time=$4

	if [ $(echo "$video_duration < ($split_time+2)"|bc) -eq 1 ];
	then
		echo "Warning: video $video_file too short, no need of splitting" >&2;
		return 2;
	fi

	rm -f ${video_file}.part0.mp4 ${video_file}.part1.mp4 ${video_file}.part00.mp4 ${video_file}.part11.mp4
	if ! (run_ffmpeg -i $video_file -f segment -segment_times $split_time -c copy -y ${video_file}.part%d.mp4);
	then
		echo "Error: failed to split video $video_file" >&2;
		return 1;
	fi

	if ! [ -f ${video_file}.part1.mp4 ];
	then
		rm -f ${video_file}.part0.mp4
		echo "Warning: video $video_file has no key frame after $split_time, failed to split" >&2;
		return 2;
	fi

	if ! [ -f ${video_file}.part0.mp4 ];
	then
		rm -f ${video_file}.part1.mp4
		echo "Error: splitting video $video_file succeeded but no video generated" >&2;
		return 1;
	fi

	if (run_ffmpeg -ss 0 -i ${video_file}.part0.mp4 -c copy -y ${video_file}.part00.mp4);
	then
		mv -f ${video_file}.part00.mp4 ${video_file}.part0.mp4
	fi

	if (run_ffmpeg -ss 0 -i ${video_file}.part1.mp4 -c copy -y ${video_file}.part11.mp4);
	then
		mv -f ${video_file}.part11.mp4 ${video_file}.part1.mp4
	fi

	durv=$(get_video_duration ${video_file}.part1.mp4)
	if [ -z "$durv" ];
	then
		echo "Error: failed to get duration for splitted video ${video_file}.part1.mp4" >&2;
		rm -f ${video_file}.part0.mp4 ${video_file}.part1.mp4;
		return 1;
	fi

	if [ $(echo "$durv < $min_time"|bc) -eq 1 ];
	then
		echo "Warning: splitted video ${video_file}.part1.mp4(duration=${durv}s) is shorter then min time of $min_time" >&2;
		rm -f ${video_file}.part0.mp4 ${video_file}.part1.mp4;
		return 2;
	fi

	return 0;
}

function gen_cross_video_dynamic_2()
{
	idx=0
	prev=
	prev_duration=
	first_split=1

	for f in `echo $inputs`;
	do
		cur=$(echo "$f"|cut -d: -f1)
		eff=$(echo "$f"|grep ':'|cut -d: -f2)
		if [ -z "$eff" ]; then
			eff=$effect
		fi
		cur_duration=$(get_video_duration $cur)
		if [ -z "$cur_duration" ];
		then
			return 1;
		fi

		if ! [ -z "$prev" ];
		then
			# merge prev + cur with eff

			# split prev video
			prev0=
			prev1=$prev;
			if [ $(echo "$prev_duration > ($duration+5)"|bc) -eq 1 ];
			then
				split_time=$(echo "$prev_duration-$duration-3"|bc)
				while (true);
				do
					split_video "$prev" $prev_duration $split_time $duration $first_split
					ret=$?
					first_split=0
					if [ $ret -eq 0 ]; # succeed
					then
						echo "Info: splitted $prev to ~.part0.mp4 and ~.part1.mp4" >&2;
						prev0=${prev}.part0.mp4;
						prev1=${prev}.part1.mp4;
						add_tmpfile "$prev0" "$prev1";
						break;
					fi
					if [ $ret -eq 1 ]; # error
					then
						return 1;
					fi
					# not splitting, adjust time and try again
					if [ $(echo "$split_time > 10"|bc) -eq 1 ];
					then
						split_time=$(echo "$split_time - 3"|bc);
						continue;
					fi
					break;
				done
			fi

			# split cur video
			cur0=$cur
			cur1=;
			if [ $(echo "$cur_duration > ($duration+5)"|bc) -eq 1 ];
			then
				split_time=$duration
				split_video "$cur" $cur_duration $split_time $duration $first_split
				ret=$?
				first_split=0
				if [ $ret -eq 0 ]; # succeed
				then
					echo "Info: splitted $cur to ~.part0.mp4 and ~.part1.mp4" >&2;
					cur0=${cur}.part0.mp4;
					cur1=${cur}.part1.mp4;
					add_tmpfile "$cur0" "$cur1";
				elif [ $ret -eq 1 ]; # error
				then
					return 1;
				fi
				# not splitting
			fi

			# merge prev1 + cur0 with eff
			adur=$(get_video_duration $prev1)
			bdur=$(get_video_duration $cur0)
			if [ -z "$adur" ] || [ -z "$bdur" ];
			then
				echo "Error: failed to get duration, $prev1(duration=$adur), $cur0(duration=$bdur)" >&2;
				return 1;
			fi
			bstart=0;
			if [ $(echo "$adur > $duration"|bc) -eq 1 ]; then
				bstart=$(echo "$adur-$duration"|bc)
			fi
			bstartms=$(echo "$bstart*1000"|bc)
			veffects=$(echo "[0:v][1:v]xfade=transition=$eff:offset=$bstart:duration=$duration[v];");
			aeffects=$(echo "[0:a]afade=t=out:st=$bstart:d=$duration:curve=tri[a1];[1:a]afade=t=in:st=0:d=$duration:curve=tri[a2];[a2]adelay=$bstartms|$bstartms[a2_2];[a1][a2_2]amix=inputs=2[a]");

			effect_ab="$cur0.effect.mp4"
			c="-ac 2"
			if [ "$first_cl" == "mono" ];
			then
				c="-ac 1"
			fi
			if ! (run_ffmpeg -i $prev1 -i $cur0 -filter_complex "${veffects}${aeffects}" -map "[v]" -map "[a]" \
			                 -framerate $first_fps -pix_fmt $first_pix -c:v libx264 -video_track_timescale $first_tb $c -c:a $first_ac -y "$effect_ab" >&2);
			then
				echo "Error: failed to generate effect video $effect_ab." >&2
				return 1;
			fi
			add_tmpfile "$effect_ab";

			if ! [ -z "$prev0" ];
			then
				echo $prev0;
			fi
			if [ -z "$cur1" ];
			then
				cur=$effect_ab;
				cur_duration=$(get_video_duration "$cur")
			else
				echo $effect_ab;
				cur=$cur1;
				cur_duration=$(get_video_duration "$cur")
			fi
		fi

		prev=$cur;
		prev_duration=$cur_duration;
		idx=$((idx+1));
	done

	echo $prev;
	return 0
}



v0=
xfiles=

if [ -z "$effect" ];
then
	xfiles=$inputs;
else
	if [ $dynamic -eq 1 ];
	then
		xfiles=$(gen_cross_video_dynamic_2);
		if (($? != 0)); then
			echo "Error: failed to execute gen_cross_video_dynamic_2." >&2;
			remove_tmpfiles;
			exit 1;
		fi
	else
		xfiles=$(gen_cross_video_static);
		if (($? != 0)); then
			echo "Error: failed to execute gen_cross_video_static." >&2;
			remove_tmpfiles;
			exit 1;
		fi
	fi
	if [ -z "$xfiles" ];
	then
		echo "Error: gen_cross_video has no output." >&2;
		remove_tmpfiles;
		exit 1;
	fi
fi

if [ "$log_level" != "error" ]; then
	echo "Concatting files: [$xfiles]"
fi

nr=$(echo "$xfiles"|wc -l)
if (($nr > 1));
then
	# fast mode, all videos must have the same framerate/width/height/codecs/samplerate/channels
	if [ $is_video_same_fmt -eq 1 ] && [ $is_audio_same_fmt -eq 1 ];
	then
		ffinputs=$output.videos.txt
		rm -f $ffinputs;
		add_tmpfile "$ffinputs";
		for f in `echo $xfiles`;
		do
			echo "file '$f'" >> $ffinputs; 
		done

		if ! (run_ffmpeg -f concat -safe 0 -i $ffinputs -c:v copy -c:a $first_ac -y "$output.1.mp4");
		then
			echo "Error executing ffmpeg for concatting videos: [$xfiles]" >&2;
			remove_tmpfiles;
			exit 1;
		fi
		# remove first black frame
		if ! (run_ffmpeg -ss 0.1 -i "$output.1.mp4" -r $first_fps -c copy -y "$output");
		then
			mv -f "$output.1.mp4" "$output";
		else
			rm -f "$output.1.mp4"
		fi
	else # slow mode, no restriction needed, all videos/audios are re-encoded
		ffinputs=
		ffmaps=
		idx=0
		for f in `echo $xfiles`;
		do
			ffinputs=$(echo "$ffinputs -i $f");
			ffmaps=$(echo "$ffmaps[$idx:0][$idx:1]");
			idx=$((idx+1));
		done

		if ! (run_ffmpeg $ffinputs -filter_complex "${ffmaps}concat=n=$idx:v=1:a=1[v][a]" -map "[v]" -map "[a]" -y "$output")
		then
			echo "Error executing ffmpeg for concatting videos: [$xfiles]" >&2;
			remove_tmpfiles;
			exit 1;
		fi
	fi
else
	mv -f $xfiles $output
fi

end_time=$(get_time_ms)
cost_time=$((end_time-start_time))
cost_time_str=$(echo $((cost_time/1000))s$((cost_time%1000))ms | sed -e s/'^0s'// -e s/'s0ms'/'s'/)
echo "All videos are merged to $output successfully, cost time: $cost_time_str"
remove_tmpfiles;
