# 3GPP EVS for Asterisk

This is an implementation of 3GPP TS 26.445 [Annex A](http://webapp.etsi.org/key/key.asp?GSMSpecPart1=26&GSMSpecPart2=445). Sometimes, 3GPP Enhanced Voice Services (EVS) are called [Full-HD Voice](http://www.iis.fraunhofer.de/en/ff/amm/prod/kommunikation/komm/evs.html). Research papers comparing EVS with other audio codecs were given at [ICASSP 2015](http://dx.doi.org/10.1109/ICASSP.2015.7178954). Further [examples…](http://www.full-hd-voice.com/en/convince-yourself.html)

To add a codec for SIP/SDP (m=, rtmap, and ftmp), you create a format module in Asterisk: `codec_evs.patch` (for m= and rtmap) and `res/res_format_attr_evs.c` (for fmtp). However, this requires both call legs to support EVS (pass-through only). If one leg does not support EVS, the call has no audio. Or, if you use the pre-recorded voice and music files of Asterisk, these files cannot be heard, because they are not in EVS but in slin. Therefore, this repository adds not just a format module for the audio-codec EVS but a transcoding module as well: `codecs/codec_evs.c`.

## Installing the patch

At least Asterisk 13.7 is required. These changes were last tested with Asterisk 13.12 (and Asterisk 14.1). If you use a newer version and the patch fails, please, [report](https://help.github.com/articles/creating-an-issue/)!

	cd /usr/src/
	wget downloads.asterisk.org/pub/telephony/asterisk/asterisk-13-current.tar.gz
	tar zxf ./asterisk*
	cd ./asterisk*
	sudo apt-get --assume-yes install build-essential autoconf libssl-dev libncurses-dev libnewt-dev libxml2-dev libsqlite3-dev uuid-dev libjansson-dev libblocksruntime-dev xmlstarlet

Apply the patch:

	wget github.com/traud/asterisk-evs/archive/master.zip
	unzip -qq master.zip
	rm master.zip
	cp --verbose --recursive ./asterisk-evs*/* ./
	patch -p0 <./codec_evs.patch

Install libraries:

If you do not want transcoding but pass-through only (because of license issues) please, skip this step. To support transcoding, you’ll need to install the 3GPP EVS reference implementation, for example in Debian/Ubuntu:

	patch -p0 <./build_evs.patch
	patch -p0 <./force_limitations.patch
	wget www.etsi.org/deliver/etsi_ts/126400_126499/126443/13.03.00_60/ts_126443v130300p0.zip
	unzip -qq ts_126443v*.zip
	unzip -qq 26443-c70_d30-ANSI-C_source_code.zip
	cd ./c-code
	chmod +r ./lib_*/*.h
	sudo mkdir /usr/include/3gpp-evs
	sudo cp --verbose --target-directory=/usr/include/3gpp-evs ./lib_*/*.h
	DEBUG=0 RELEASE=1 CFLAGS='-NDEBUG -fPIC' make
	cd ./build
	rm ./decoder.o
	gcc -shared -o lib3gpp-evs.so *.o
	sudo cp ./lib3gpp-evs.so /usr/lib/
	cd /usr/src/asterisk*

Run the bootstrap script to re-generate configure:

	./bootstrap.sh

Configure your patched Asterisk:

	./configure

Enable slin16 in menuselect for transcoding, for example via:

	make menuselect.makeopts
	./menuselect/menuselect --enable-category MENUSELECT_CORE_SOUNDS

Compile and install:

	make
	sudo make install

## Testing

Currently, I am not aware of any other VoIP/SIP project offering EVS. Consequently, you have to patch two Asterisk servers and run EVS between those. My main objective was to play around, test, and learn more about EVS.

Although the Qualcomm Snapdragon 820 (and newer) chipset(s) offer EVS, not all phones come with EVS enabled. Not all of the remaing phones offer the built-in VoIP/SIP client of the Android Open Source Platform (AOSP). Finally, I am not aware of one phone which bridges the EVS capabilities between the hardware chipset and the software client.

The Rohde & Schwarz CMW500 can be extended for EVS – however here, this transcoding module was not tested with that either.

## What is missing

Although this list is rather long, these features are disabled at SDP negotiation via the `force_limitations.patch` and should not create an interoperability issue.

* DTX/VAD/CN: Silence Insertion Descriptor (SID) frames are untested.
* VBR: The lowest mode is variable in bitrate (average 5.9 kbit/s) but requires DTX.
* Compound payload: Several frames per payload, for example when FEC or a packetization time (ptime) longer than 20 ms are used. This is useful to lower the overall overhead (RTP, UDP, and IP) of the lowest mode even further.
* Packet-Loss Concealment (native PLC), see [ASTERISK-25629…](http://issues.asterisk.org/jira/browse/ASTERISK-25629)
* Channel Awareness (RTCP interaction), see [ASTERISK-26584…](http://issues.asterisk.org/jira/browse/ASTERISK-26584)
* Compact Format mode; not sure if that is possible with Asterisk, see `codec_evs.c:evs_sample_counter`
* AMR-WB IO without transcoding

The transcoding module works for me and contains everything I need. If you cannot code yourself, however, a feature is missing for you, please, [report](https://help.github.com/articles/creating-an-issue/) and send me at least a testing device.

## Thanks go to

everyone who made the 3GPP EVS reference implementation possible.