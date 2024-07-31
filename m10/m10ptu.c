
/* big endian forest
 *
 * gcc m10ptu.c -lm -o m10ptu
 * M10 w/ trimble GPS
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef CYGWIN
  #include <fcntl.h>  // cygwin: _setmode()
  #include <io.h>
#endif

typedef unsigned char  ui8_t;
typedef unsigned short ui16_t;
typedef unsigned int   ui32_t;
typedef          int    i32_t;

typedef struct {
    int week; int gpssec;
    int jahr; int monat; int tag;
    int wday;
    int std; int min; int sek;
    double lat; double lon; double alt;
    double vH; double vD; double vV;
    double vx; double vy; double vD2;
    char SN[12];
} datum_t;

datum_t datum;

int option_verbose = 0,  // ausfuehrliche Anzeige
    option_raw = 0,      // rohe Frames
    option_inv = 0,      // invertiert Signal
    option_res = 0,      // genauere Bitmessung
    option_avg = 0,      // moving average
    option_b = 0,
    option_color = 0,
    option_ptu = 0,
    option_sat = 0,
    wavloaded = 0;
int wav_channel = 0;     // audio channel: left


/* -------------------------------------------------------------------------- */
/*
 * Convert GPS Week and Seconds to Modified Julian Day.
 * - Adapted from sci.astro FAQ.
 * - Ignores UTC leap seconds.
 */
void Gps2Date(long GpsWeek, long GpsSeconds, int *Year, int *Month, int *Day) {

    long GpsDays, Mjd;
    long J, C, Y, M;

    GpsDays = GpsWeek * 7 + (GpsSeconds / 86400);
    Mjd = 44244 + GpsDays;

    J = Mjd + 2468570;
    C = 4 * J / 146097;
    J = J - (146097 * C + 3) / 4;
    Y = 4000 * (J + 1) / 1461001;
    J = J - 1461 * Y / 4 + 31;
    M = 80 * J / 2447;
    *Day = J - 2447 * M / 80;
    J = M / 11;
    *Month = M + 2 - (12 * J);
    *Year = 100 * (C - 49) + Y + J;
}
/* -------------------------------------------------------------------------- */

/*
   alternative Demodulation: M10 problematisch
   bits_per_sample klein, sync laeuft auseinander
   exakte Baudrate entscheidend
   9600 baud -> 9616 baud ?
*/

// option_b: exakte Baudrate wichtig!
// im Prinzip in sync-preamble ermittelbar
#define BAUD_RATE   9616 //2*4800

int sample_rate = 0, bits_sample = 0, channels = 0;
float samples_per_bit = 0;

int findstr(char *buf, char *str, int pos) {
    int i;
    for (i = 0; i < 4; i++) {
        if (buf[(pos+i)%4] != str[i]) break;
    }
    return i;
}

int read_wav_header(FILE *fp) {
    char txt[4+1] = "\0\0\0\0";
    unsigned char dat[4];
    int byte, p=0;

    if (fread(txt, 1, 4, fp) < 4) return -1;
    if (strncmp(txt, "RIFF", 4)) return -1;
    if (fread(txt, 1, 4, fp) < 4) return -1;
    // pos_WAVE = 8L
    if (fread(txt, 1, 4, fp) < 4) return -1;
    if (strncmp(txt, "WAVE", 4)) return -1;
    // pos_fmt = 12L
    for ( ; ; ) {
        if ( (byte=fgetc(fp)) == EOF ) return -1;
        txt[p % 4] = byte;
        p++; if (p==4) p=0;
        if (findstr(txt, "fmt ", p) == 4) break;
    }
    if (fread(dat, 1, 4, fp) < 4) return -1;
    if (fread(dat, 1, 2, fp) < 2) return -1;

    if (fread(dat, 1, 2, fp) < 2) return -1;
    channels = dat[0] + (dat[1] << 8);

    if (fread(dat, 1, 4, fp) < 4) return -1;
    memcpy(&sample_rate, dat, 4); //sample_rate = dat[0]|(dat[1]<<8)|(dat[2]<<16)|(dat[3]<<24);

    if (fread(dat, 1, 4, fp) < 4) return -1;
    if (fread(dat, 1, 2, fp) < 2) return -1;
    //byte = dat[0] + (dat[1] << 8);

    if (fread(dat, 1, 2, fp) < 2) return -1;
    bits_sample = dat[0] + (dat[1] << 8);

    // pos_dat = 36L + info
    for ( ; ; ) {
        if ( (byte=fgetc(fp)) == EOF ) return -1;
        txt[p % 4] = byte;
        p++; if (p==4) p=0;
        if (findstr(txt, "data", p) == 4) break;
    }
    if (fread(dat, 1, 4, fp) < 4) return -1;


    fprintf(stderr, "sample_rate: %d\n", sample_rate);
    fprintf(stderr, "bits       : %d\n", bits_sample);
    fprintf(stderr, "channels   : %d\n", channels);

    if ((bits_sample != 8) && (bits_sample != 16)) return -1;

    samples_per_bit = sample_rate/(float)BAUD_RATE;

    fprintf(stderr, "samples/bit: %.2f\n", samples_per_bit);

    return 0;
}


#define EOF_INT  0x1000000

#define LEN_movAvg 3
int movAvg[LEN_movAvg];
unsigned long sample_count = 0;
double bitgrenze = 0;

int read_signed_sample(FILE *fp) {  // int = i32_t
    int byte, i, sample=0, s=0;     // EOF -> 0x1000000

    for (i = 0; i < channels; i++) {
                           // i = 0: links bzw. mono
        byte = fgetc(fp);
        if (byte == EOF) return EOF_INT;
        if (i == wav_channel) sample = byte;

        if (bits_sample == 16) {
            byte = fgetc(fp);
            if (byte == EOF) return EOF_INT;
            if (i == wav_channel) sample +=  byte << 8;
        }

    }

    if (bits_sample ==  8)  s = sample-128;   // 8bit: 00..FF, centerpoint 0x80=128
    if (bits_sample == 16)  s = (short)sample;

    if (option_avg) {
        movAvg[sample_count % LEN_movAvg] = s;
        s = 0;
        for (i = 0; i < LEN_movAvg; i++) s += movAvg[i];
        s = (s+0.5) / LEN_movAvg;
    }

    sample_count++;

    return s;
}

int par=1, par_alt=1;

int read_bits_fsk(FILE *fp, int *bit, int *len) {
    static int sample;
    int n, y0;
    float l, x1;
    static float x0;

    n = 0;
    do{
        y0 = sample;
        sample = read_signed_sample(fp);
        if (sample == EOF_INT) return EOF;
        //sample_count++; // in read_signed_sample()
        par_alt = par;
        par =  (sample >= 0) ? 1 : -1;    // 8bit: 0..127,128..255 (-128..-1,0..127)
        n++;
    } while (par*par_alt > 0);

    if (!option_res) l = (float)n / samples_per_bit;
    else {                                 // genauere Bitlaengen-Messung
        x1 = sample/(float)(sample-y0);    // hilft bei niedriger sample rate
        l = (n+x0-x1) / samples_per_bit;   // meist mehr frames (nicht immer)
        x0 = x1;
    }

    *len = (int)(l+0.5);

    if (!option_inv) *bit = (1+par_alt)/2;  // oben 1, unten -1
    else             *bit = (1-par_alt)/2;  // sdr#<rev1381?, invers: unten 1, oben -1
// *bit = (1+inv*par_alt)/2; // ausser inv=0

    /* Y-offset ? */

    return 0;
}

int bitstart = 0;
unsigned long scount = 0;
int read_rawbit(FILE *fp, int *bit) {
    int sample;
    int sum;

    sum = 0;

    if (bitstart) {
        scount = 0;    // eigentlich scount = 1
        bitgrenze = 0; //   oder bitgrenze = -1
        bitstart = 0;
    }
    bitgrenze += samples_per_bit;

    do {
        sample = read_signed_sample(fp);
        if (sample == EOF_INT) return EOF;
        //sample_count++; // in read_signed_sample()
        //par =  (sample >= 0) ? 1 : -1;    // 8bit: 0..127,128..255 (-128..-1,0..127)
        sum += sample;
        scount++;
    } while (scount < bitgrenze);  // n < samples_per_bit

    if (sum >= 0) *bit = 1;
    else          *bit = 0;

    if (option_inv) *bit ^= 1;

    return 0;
}

int read_rawbit2(FILE *fp, int *bit) {
    int sample;
    int sum;

    sum = 0;

    if (bitstart) {
        scount = 0;    // eigentlich scount = 1
        bitgrenze = 0; //   oder bitgrenze = -1
        bitstart = 0;
    }

    bitgrenze += samples_per_bit;
    do {
        sample = read_signed_sample(fp);
        if (sample == EOF_INT) return EOF;
        //sample_count++; // in read_signed_sample()
        //par =  (sample >= 0) ? 1 : -1;    // 8bit: 0..127,128..255 (-128..-1,0..127)
        sum += sample;
        scount++;
    } while (scount < bitgrenze);  // n < samples_per_bit

    bitgrenze += samples_per_bit;
    do {
        sample = read_signed_sample(fp);
        if (sample == EOF_INT) return EOF;
        //sample_count++; // in read_signed_sample()
        //par =  (sample >= 0) ? 1 : -1;    // 8bit: 0..127,128..255 (-128..-1,0..127)
        sum -= sample;
        scount++;
    } while (scount < bitgrenze);  // n < samples_per_bit

    if (sum >= 0) *bit = 1;
    else          *bit = 0;

    if (option_inv) *bit ^= 1;

    return 0;
}

/* -------------------------------------------------------------------------- */

/*
Header = Sync-Header + Sonde-Header:
1100110011001100 1010011001001100  1101010011010011 0100110101010101 0011010011001100
uudduudduudduudd ududduuddudduudd  uudududduududduu dudduudududududu dduududduudduudd (oder:)
dduudduudduudduu duduudduuduudduu  ddududuudduduudd uduuddududududud uudduduudduudduu (komplement)
 0 0 0 0 0 0 0 0  1 1 - - - 0 0 0   0 1 1 0 0 1 0 0  1 0 0 1 1 1 1 1  0 0 1 0 0 0 0 0
*/

#define BITS 8
#define HEADLEN 32  // HEADLEN+HEADOFS=32 <= strlen(header)
#define HEADOFS  0
                 // Sync-Header (raw)               // Sonde-Header (bits)
//char head[] = "11001100110011001010011001001100"; //"011001001001111100100000"; // M10: 64 9F , M2K2: 64 8F
                                                    //"011101101001111100100000"; // M10: 76 9F , aux-data?
                                                    //"011001000100100100001001"; // M10-dop: 64 49
char header[] =  "10011001100110010100110010011001";

#define FRAME_LEN       (100+1)   // 0x64+1
#define BITFRAME_LEN    (FRAME_LEN*BITS)
#define RAWBITFRAME_LEN (BITFRAME_LEN*2)

char buf[HEADLEN];
int bufpos = -1;


#define FRAMESTART 0
#define AUX_LEN        20
#define BITAUX_LEN    (AUX_LEN*BITS)
#define RAWBITAUX_LEN (BITAUX_LEN*2)

ui8_t frame_bytes[FRAME_LEN+AUX_LEN+2];
char frame_rawbits[RAWBITFRAME_LEN+RAWBITAUX_LEN+16];  // frame_rawbits-32="11001100110011001010011001001100";
char frame_bits[BITFRAME_LEN+BITAUX_LEN+8];

int auxlen = 0; // 0 .. 0x76-0x64


void inc_bufpos() {
  bufpos = (bufpos+1) % HEADLEN;
}

char cb_inv(char c) {
    if (c == '0') return '1';
    if (c == '1') return '0';
    return c;
}

// Gefahr bei Manchester-Codierung: inverser Header wird leicht fehl-erkannt
// da manchester1 und manchester2 nur um 1 bit verschoben
int compare2() {
    int i, j;

    i = 0;
    j = bufpos;
    while (i < HEADLEN) {
        if (j < 0) j = HEADLEN-1;
        if (buf[j] != header[HEADOFS+HEADLEN-1-i]) break;
        j--;
        i++;
    }
    if (i == HEADLEN) return 1;

    i = 0;
    j = bufpos;
    while (i < HEADLEN) {
        if (j < 0) j = HEADLEN-1;
        if (buf[j] != cb_inv(header[HEADOFS+HEADLEN-1-i])) break;
        j--;
        i++;
    }
    if (i == HEADLEN) return -1;

    return 0;

}

int bits2bytes(char *bitstr, ui8_t *bytes) {
    int i, bit, d, byteval;
    int bitpos, bytepos;

    bitpos = 0;
    bytepos = 0;

    while (bytepos < FRAME_LEN+AUX_LEN) {

        byteval = 0;
        d = 1;
        for (i = 0; i < BITS; i++) {
            //bit=*(bitstr+bitpos+i); /* little endian */
            bit=*(bitstr+bitpos+7-i);  /* big endian */
            // bit == 'x' ?
            if         (bit == '1')                     byteval += d;
            else /*if ((bit == '0') || (bit == 'x'))*/  byteval += 0;
            d <<= 1;
        }
        bitpos += BITS;
        bytes[bytepos++] = byteval & 0xFF;

    }

    //while (bytepos < FRAME_LEN+AUX_LEN) bytes[bytepos++] = 0;

    return 0;
}

/* -------------------------------------------------------------------------- */
// PSK  (bzw. biphase-M (oder differential Manchester?))
// nach Synchronisation: 00,11->0 ; 01,10->1 (Phasenwechsel)
void psk_bpm(char* frame_rawbits, char *frame_bits) {
    int i;
    char bit;
    //int err = 0;

    for (i = 0; i < BITFRAME_LEN+BITAUX_LEN; i++) {

        //if (i > 0 && (frame_rawbits[2*i] == frame_rawbits[2*i-1])) err = 1;

        if (frame_rawbits[2*i] == frame_rawbits[2*i+1]) bit = '0';
        else                                            bit = '1';

        //if (err) frame_bits[i] = 'x'; else
        frame_bits[i] = bit;
        //err = 0;

    }
}

/*
Header = Sync-Header + Sonde-Header:
1100110011001100 1010011001001100  1101010011010011 0100110101010101 0011010011001100
uudduudduudduudd ududduuddudduudd  uudududduududduu dudduudududududu dduududduudduudd (oder:)
dduudduudduudduu duduudduuduudduu  ddududuudduduudd uduuddududududud uudduduudduudduu (komplement)
 0 0 0 0 0 0 0 0  1 1 - - - 0 0 0   0 1 1 0 0 1 0 0  1 0 0 1 1 1 1 1  0 0 1 0 0 0 0 0
*/
/*
110101001101001101001101010101010011010011001100
 101010011010011010011010101010100110100110011001011001100101011001100110011001100110011001100101100110011001100110011001100101010101010101010101010101101010100110011010011001011001011010101010010101100110100110010101100110011001011001100110100101011010101001100101010101101010011001010101100110010110100110010101100101100101100110101010011001100110010110011001100110011001100110100101011010101001101010100101100101100110011001100110011001100110011001100110011001011001101010011001100110011010101001100101100110100110011001010101101001100110010110011010100110100110011010100110011001010101101001100110010101100110011010101001100110101001100110011001100101100110011001100110011001100110011001100110011001100110011001100110011001100110011001100110011010011010100110100110011001100110011001100110011001100101101001011010100101101001101001100110100110100110010110010110101010010110010110011001011001010101010101010110011001100110011010011010100110011001011001100110011001100110011001100110011001100110011001100110101001011010011010010110010110010101010110010110011001101001101010101010011010100110011010011010011010100110101001100110011010100110010110011001010110101001101001100110100101011001100110011010011001100110011001100110010101011001100110011001100110100110101001100110011001100110011001100110011001100110011001100110011001100110011001100110011001100110011010101001101001011001100101010101100110101001011001100110011001100110101001010110011001100110010110011001010110011001100110011001100110011001011001100101011010100101100110010110101001101001011001101001011001101010011010010110101001010101100110011001101010100110011001100000
*/
int dpsk_bpm(char* frame_rawbits, char *frame_bits, int len) {
    int i;
    char bit;
    char bit0;
    //int err = 0;

    bit0 = (frame_rawbits[0] & 1) ^ 1;

    for (i = 0; i < len/2; i++) {

        if ((frame_rawbits[2*i  ] & 1) == 1 &&
            (frame_rawbits[2*i+1] & 1) == 0   ) bit = 1;
   else if ((frame_rawbits[2*i  ] & 1) == 0 &&
            (frame_rawbits[2*i+1] & 1) == 1   ) bit = 0;
        else {
                bit = 2;
                frame_bits[i] = 'x';
                bit0 = bit&1;
                continue;
                //err = 1;
             }

        if (bit0 == bit) frame_bits[i] = '1';
        else             frame_bits[i] = '0';
// frame_bits[i] = 0x31 ^ (bit0 ^ bit);

        bit0 = bit;

    }

    return bit0;
}

/* ------------------------------------------------------------------------------------------------------------- */

/*
M10 w/ trimble GPS

frame[0x0] = framelen
frame[0x1] = 0x9F (type M10)

init/noGPS: frame[0x2]=0x23
GPS: frame[0x2]=0x20 (GPS trimble pck 0x8F-20 sub-id)

frame[0x02..0x21] = GPS trimble pck 0x8F-20 byte 0..31 (sub-id, vel, tow, lat, lon, alt, fix, NumSV, UTC-ofs, week)
frame[0x22..0x2D] = GPS trimble pck 0x8F-20 byte 32..55:2 (PRN 1..12 only)

Trimble Copernicus II
GPS packet 0x8F-20 (p.138)
byte
0      sub-pck id (always 0x20)
2-3    velE (i16) 0.005m/s
4-5    velN (i16) 0.005m/s
6-7    velU (i16) 0.005m/s
8-11   TOW (ms)
12-15  lat (scale 2^32/360) (i32) -90..90
16-19  lon (scale 2^32/360) (ui32) 0..360 <-> (i32) -180..180
20-23  alt (i32) mm above ellipsoid)
24     bit0: vel-scale (0: 0.005m/s)
26     datum (1: WGS-84)
27     fix: bit0(0:valid fix, 1:invalid fix), bit2(0:3D, 1:2D)
28     numSVs
29     UTC offset = (GPS - UTC) sec
30-31  GPS week
32+2*n PRN_(n+1), bit0-5

frame[0x32..0x5C] sensors (rel.hum., temp.)
frame[0x5D..0x61] SN
frame[0x62] counter
frame[0x63..0x64] check  (AUX len=0x76: frame[0x63..0x74], frame[0x75..0x76])


6449/10sec-frame:
GPS trimble pck 0x47 (signal levels): numSats sat1 lev1 sat2 lev2 ..
frame[0x0] = framelen
frame[0x1] = 0x49
frame[0x2] = numSats (max 12)
frame[0x3+2*n] = PRN_(n+1)
frame[0x4+2*n] = signal level (float32 -> i8-byte level)

*/


#define stdFLEN        0x64  // pos[0]=0x64
#define pos_GPSTOW     0x0A  // 4 byte
#define pos_GPSlat     0x0E  // 4 byte
#define pos_GPSlon     0x12  // 4 byte
#define pos_GPSalt     0x16  // 4 byte
#define pos_GPSweek    0x20  // 2 byte
//Velocity East-North-Up (ENU)
#define pos_GPSvE      0x04  // 2 byte
#define pos_GPSvN      0x06  // 2 byte
#define pos_GPSvU      0x08  // 2 byte
#define pos_SN         0x5D  // 2+3 byte
#define pos_Check     (stdFLEN-1)  // 2 byte


#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

#define XTERM_COLOR_BROWN   "\x1b[38;5;94m"  // 38;5;{0..255}m

#define col_GPSweek    "\x1b[38;5;20m"  // 2 byte
#define col_GPSTOW     "\x1b[38;5;27m"  // 4 byte
#define col_GPSdate    "\x1b[38;5;94m" //111
#define col_GPSlat     "\x1b[38;5;34m"  // 4 byte
#define col_GPSlon     "\x1b[38;5;70m"  // 4 byte
#define col_GPSalt     "\x1b[38;5;82m"  // 4 byte
#define col_GPSvel     "\x1b[38;5;36m"  // 6 byte
#define col_SN         "\x1b[38;5;58m"  // 3 byte
#define col_Check      "\x1b[38;5;11m"  // 2 byte
#define col_TXT        "\x1b[38;5;244m"
#define col_FRTXT      "\x1b[38;5;244m"
#define col_CSok       "\x1b[38;5;2m"
#define col_CSno       "\x1b[38;5;1m"

/*
$ for code in  {0..255}
> do echo -e "\e[38;5;${code}m"'\\e[38;5;'"$code"m"\e[0m"
> done
*/

int get_GPSweek() {
    int i;
    unsigned byte;
    ui8_t gpsweek_bytes[2];
    int gpsweek;

    for (i = 0; i < 2; i++) {
        byte = frame_bytes[pos_GPSweek + i];
        gpsweek_bytes[i] = byte;
    }

    gpsweek = (gpsweek_bytes[0] << 8) + gpsweek_bytes[1];
    datum.week = gpsweek;

    if (gpsweek < 0 || gpsweek > 3000) return -1;

    return 0;
}

char weekday[7][3] = { "So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};

int get_GPStime() {
    int i;
    unsigned byte;
    ui8_t gpstime_bytes[4];
    int gpstime, day; // int ms;

    for (i = 0; i < 4; i++) {
        byte = frame_bytes[pos_GPSTOW + i];
        gpstime_bytes[i] = byte;
    }

    gpstime = 0;
    for (i = 0; i < 4; i++) {
        gpstime |= gpstime_bytes[i] << (8*(3-i));
    }

    //ms = gpstime % 1000;
    gpstime /= 1000;
    datum.gpssec = gpstime;

    day = gpstime / (24 * 3600);
    gpstime %= (24*3600);

    if ((day < 0) || (day > 6)) return -1;
    datum.wday = day;
    datum.std = gpstime/3600;
    datum.min = (gpstime%3600)/60;
    datum.sek = gpstime%60;

    return 0;
}

double B60B60 = 0xB60B60;  // 2^32/360 = 0xB60B60.xxx

int get_GPSlat() {
    int i;
    unsigned byte;
    ui8_t gpslat_bytes[4];
    i32_t gpslat;
    double lat;

    for (i = 0; i < 4; i++) {
        byte = frame_bytes[pos_GPSlat + i];
        gpslat_bytes[i] = byte;
    }

    gpslat = 0;
    for (i = 0; i < 4; i++) {
        gpslat |= gpslat_bytes[i] << (8*(3-i));
    }
    lat = gpslat / B60B60;
    datum.lat = lat;

    return 0;
}

int get_GPSlon() {
    int i;
    unsigned byte;
    ui8_t gpslon_bytes[4];
    i32_t gpslon;  // (ui32) 0..360 <-> (i32) -180..180
    double lon;

    for (i = 0; i < 4; i++) {
        byte = frame_bytes[pos_GPSlon + i];
        gpslon_bytes[i] = byte;
    }

    gpslon = 0;
    for (i = 0; i < 4; i++) {
        gpslon |= gpslon_bytes[i] << (8*(3-i));
    }
    lon = gpslon / B60B60;
    datum.lon = lon;

    return 0;
}

int get_GPSalt() {
    int i;
    unsigned byte;
    ui8_t gpsalt_bytes[4];
    i32_t gpsalt;
    double alt;

    for (i = 0; i < 4; i++) {
        byte = frame_bytes[pos_GPSalt + i];
        gpsalt_bytes[i] = byte;
    }

    gpsalt = 0;
    for (i = 0; i < 4; i++) {
        gpsalt |= gpsalt_bytes[i] << (8*(3-i));
    }
    alt = gpsalt / 1000.0;
    datum.alt = alt;

    return 0;
}

int get_GPSvel() {
    int i;
    unsigned byte;
    ui8_t gpsVel_bytes[2];
    short vel16; // i16_t
    double vx, vy, dir, alpha;
    const double sc5 = 2e2;  // scale 0.005m/s

    for (i = 0; i < 2; i++) {
        byte = frame_bytes[pos_GPSvE + i];
        gpsVel_bytes[i] = byte;
    }
    vel16 = gpsVel_bytes[0] << 8 | gpsVel_bytes[1];
    vx = vel16 / sc5; // ost

    for (i = 0; i < 2; i++) {
        byte = frame_bytes[pos_GPSvN + i];
        gpsVel_bytes[i] = byte;
    }
    vel16 = gpsVel_bytes[0] << 8 | gpsVel_bytes[1];
    vy= vel16 / sc5; // nord

    datum.vx = vx;
    datum.vy = vy;
    datum.vH = sqrt(vx*vx+vy*vy);
///*
    alpha = atan2(vy, vx)*180/M_PI;  // ComplexPlane (von x-Achse nach links) - GeoMeteo (von y-Achse nach rechts)
    dir = 90-alpha;                  // z=x+iy= -> i*conj(z)=y+ix=re(i(pi/2-t)), Achsen und Drehsinn vertauscht
    if (dir < 0) dir += 360;         // atan2(y,x)=atan(y/x)=pi/2-atan(x/y) , atan(1/t) = pi/2 - atan(t)
    datum.vD2 = dir;
//*/
    dir = atan2(vx, vy) * 180 / M_PI;
    if (dir < 0) dir += 360;
    datum.vD = dir;

    for (i = 0; i < 2; i++) {
        byte = frame_bytes[pos_GPSvU + i];
        gpsVel_bytes[i] = byte;
    }
    vel16 = gpsVel_bytes[0] << 8 | gpsVel_bytes[1];
    datum.vV = vel16 / sc5;

    return 0;
}

int get_SN() {
    int i;
    unsigned byte;
    ui8_t sn_bytes[5];

    for (i = 0; i < 11; i++) datum.SN[i] = ' '; datum.SN[11] = '\0';

    for (i = 0; i < 5; i++) {
        byte = frame_bytes[pos_SN + i];
        sn_bytes[i] = byte;
    }

    byte = sn_bytes[2];
    sprintf(datum.SN, "%1X%02u", (byte>>4)&0xF, byte&0xF);
    byte = sn_bytes[3] | (sn_bytes[4]<<8);
    sprintf(datum.SN+3, " %1X %1u%04u", sn_bytes[0]&0xF, (byte>>13)&0x7, byte&0x1FFF);

    return 0;
}

/* -------------------------------------------------------------------------- */
/*
g : F^n -> F^16      // checksum, linear
g(m||b) = f(g(m),b)

// update checksum
f : F^16 x F^8 -> F^16 linear

010100001000000101000000
001010000100000010100000
000101000010000001010000
000010100001000000101000
000001010000100000010100
100000100000010000001010
000000011010100000000100
100000000101010000000010
000000001000000000000000
000000000100000000000000
000000000010000000000000
000000000001000000000000
000000000000100000000000
000000000000010000000000
000000000000001000000000
000000000000000100000000
*/

int update_checkM10(int c, ui8_t b) {
    int c0, c1, t, t6, t7, s;

    c1 = c & 0xFF;

    // B
    b  = (b >> 1) | ((b & 1) << 7);
    b ^= (b >> 2) & 0xFF;

    // A1
    t6 = ( c     & 1) ^ ((c>>2) & 1) ^ ((c>>4) & 1);
    t7 = ((c>>1) & 1) ^ ((c>>3) & 1) ^ ((c>>5) & 1);
    t = (c & 0x3F) | (t6 << 6) | (t7 << 7);

    // A2
    s  = (c >> 7) & 0xFF;
    s ^= (s >> 2) & 0xFF;


    c0 = b ^ t ^ s;

    return ((c1<<8) | c0) & 0xFFFF;
}

int checkM10(ui8_t *msg, int len) {
    int i, cs;

    cs = 0;
    for (i = 0; i < len; i++) {
        cs = update_checkM10(cs, msg[i]);
    }

    return cs & 0xFFFF;
}

/* -------------------------------------------------------------------------- */

// https://www.gruan.org/gruan/editor/documents/meetings/icm-6/pres/pres_306_Haeffelin.pdf
//
// Temperature Sensor
// NTC-Thermistor Shibaura PB5-41E
//
float get_Temp(int csOK) {
// NTC-Thermistor Shibaura PB5-41E
// T00 = 273.15 +  0.0 , R00 = 15e3
// T25 = 273.15 + 25.0 , R25 = 5.369e3
// B00 = 3450.0 Kelvin // 0C..100C, poor fit low temps
// [  T/C  , R/1e3 ] ( [P__-43]/2.0 ):
// [ -50.0 , 204.0 ]
// [ -45.0 , 150.7 ]
// [ -40.0 , 112.6 ]
// [ -35.0 , 84.90 ]
// [ -30.0 , 64.65 ]
// [ -25.0 , 49.66 ]
// [ -20.0 , 38.48 ]
// [ -15.0 , 30.06 ]
// [ -10.0 , 23.67 ]
// [  -5.0 , 18.78 ]
// [   0.0 , 15.00 ]
// [   5.0 , 12.06 ]
// [  10.0 , 9.765 ]
// [  15.0 , 7.955 ]
// [  20.0 , 6.515 ]
// [  25.0 , 5.370 ]
// [  30.0 , 4.448 ]
// [  35.0 , 3.704 ]
// [  40.0 , 3.100 ]
// -> Steinhart–Hart coefficients (polyfit):
    float p0 = 1.07303516e-03,
          p1 = 2.41296733e-04,
          p2 = 2.26744154e-06,
          p3 = 6.52855181e-08;
// T/K = 1/( p0 + p1*ln(R) + p2*ln(R)^2 + p3*ln(R)^3 )

    // range/scale 0, 1, 2:                        // M10-pcb
    float Rs[3] = { 12.1e3 ,  36.5e3 ,  475.0e3 }; // bias/series
    float Rp[3] = { 1e20   , 330.0e3 , 2000.0e3 }; // parallel, Rp[0]=inf

    ui8_t  scT;     // {0,1,2}, range/scale voltage divider
    ui16_t ADC_RT;  // ADC12 P6.7(A7) , adr_0377h,adr_0376h
    ui16_t Tcal[2]; // adr_1000h[scT*4]

    float adc_max = 4095.0; // ADC12
    float x, R;
    float T = 0;    // T/Kelvin

    scT     =  frame_bytes[0x3E]; // adr_0455h
    ADC_RT  = (frame_bytes[0x40] << 8) | frame_bytes[0x3F];
    ADC_RT -= 0xA000;
    Tcal[0] = (frame_bytes[0x42] << 8) | frame_bytes[0x41];
    Tcal[1] = (frame_bytes[0x44] << 8) | frame_bytes[0x43];

    x = (adc_max-ADC_RT)/ADC_RT;  // (Vcc-Vout)/Vout
    if (scT < 3) R =  Rs[scT] /( x - Rs[scT]/Rp[scT] );
    else         R = -1;

    if (R > 0)  T =  1/( p0 + p1*log(R) + p2*log(R)*log(R) + p3*log(R)*log(R)*log(R) );

    if (option_verbose >= 3 && csOK) { // on-chip temperature
        ui16_t ADC_Ti_raw = (frame_bytes[0x49] << 8) | frame_bytes[0x48]; // int.temp.diode, ref: 4095->1.5V
        float vti, ti;
        // INCH1A (temp.diode), slau144
        vti = ADC_Ti_raw/4095.0 * 1.5; // V_REF+ = 1.5V, no calibration
        ti = (vti-0.986)/0.00355;      // 0.986/0.00355=277.75, 1.5/4095/0.00355=0.1032
        fprintf(stdout, "  (Ti:%.1fC)", ti);
        // SegmentA-Calibration:
        //ui16_t T30 = adr_10e2h; // CAL_ADC_15T30
        //ui16_t T85 = adr_10e4h; // CAL_ADC_15T85
        //float  tic = (ADC_Ti_raw-T30)*(85.0-30.0)/(T85-T30) + 30.0;
        //fprintf(stdout, "  (Tic:%.1fC)", tic);
    }

    return  T - 273.15; // Celsius
}
/*
frame[0x32]: adr_1074h
frame[0x33]: adr_1075h
frame[0x34]: adr_1076h

frame[0x35..0x37]: TBCCR1 ; relHumCap-freq

frame[0x38]: adr_1078h
frame[0x39]: adr_1079h
frame[0x3A]: adr_1077h
frame[0x3B]: adr_100Ch
frame[0x3C..3D]: 0


frame[0x3E]: scale_index ; scale/range-index
frame[0x3F..40] = ADC12_A7 | 0xA000, V_R+=AVcc ; Thermistor

frame[0x41]: adr_1000h[scale_index*4]
frame[0x42]: adr_1000h[scale_index*4+1]
frame[0x43]: adr_1000h[scale_index*4+2]
frame[0x44]: adr_1000h[scale_index*4+3]

frame[0x45..46]: ADC12_A5/4, V_R+=2.5V
frame[0x47]: ADC12_A2/16 , V_R+=2.5V
frame[0x48..49]: ADC12_iT, V_R+=1.5V (int.Temp.diode)
frame[0x4C..4D]: ADC12_A6, V_R+=2.5V
frame[0x4E..4F]: ADC12_A3, V_R+=AVcc
frame[0x50..54]: 0;
frame[0x55..56]: ADC12_A1, V_R+=AVcc
frame[0x57..58]: ADC12_A0, V_R+=AVcc
frame[0x59..5A]: ADC12_A4, V_R+=AVcc  // ntc2: R(25C)=2.2k, Rs=22.1e3 (relHumCap-Temp)

frame[0x5B]:
frame[0x5C]: adr_108Eh


frame[0x5D]: adr_1082h (SN)
frame[0x5E]: adr_1083h (SN)
frame[0x5F]: adr_1084h (SN)
frame[0x60]: adr_1080h (SN)
frame[0x61]: adr_1081h (SN)
*/
float get_Tntc2(int csOK) {
// SMD ntc
    float Rs = 22.1e3;          // P5.6=Vcc
//  float R25 = 2.2e3;
//  float b = 3650.0;           // B/Kelvin
//  float T25 = 25.0 + 273.15;  // T0=25C, R0=R25=5k
// -> Steinhart–Hart coefficients (polyfit):
    float p0 =  4.42606809e-03,
          p1 = -6.58184309e-04,
          p2 =  8.95735557e-05,
          p3 = -2.84347503e-06;
    float T = 0.0;              // T/Kelvin
    ui16_t ADC_ntc2;            // ADC12 P6.4(A4)
    float x, R;
    if (csOK)
    {
        ADC_ntc2  = (frame_bytes[0x5A] << 8) | frame_bytes[0x59];
        x = (4095.0 - ADC_ntc2)/ADC_ntc2;  // (Vcc-Vout)/Vout
        R = Rs / x;
        //if (R > 0)  T = 1/(1/T25 + 1/b * log(R/R25));
        if (R > 0)  T =  1/( p0 + p1*log(R) + p2*log(R)*log(R) + p3*log(R)*log(R)*log(R) );
    }
    return T - 273.15;
}

// Humidity Sensor
// U.P.S.I.
//
#define FREQ_CAPCLK (8e6/2)      // 8 MHz XT2 crystal, InputDivider IDx=01 (/2)
#define LN2         0.693147181
#define ADR_108A    1000.0       // 0x3E8=1000

float get_count_55() { // CalRef 55%RH , T=20C ?
    ui32_t TBCREF_1000 = frame_bytes[0x32] | (frame_bytes[0x33]<<8) | (frame_bytes[0x34]<<16);
    return TBCREF_1000 / ADR_108A;
}
float get_count_RH() {  // capture 1000 rising edges
    ui32_t TBCCR1_1000 = frame_bytes[0x35] | (frame_bytes[0x36]<<8) | (frame_bytes[0x37]<<16);
    return TBCCR1_1000 / ADR_108A;
}
float get_TLC555freq(float count) {
    return FREQ_CAPCLK / count;
}

float get_C_RH(float freq, float T) {  // TLC555 astable: R_A=3.65k, R_B=338k
    float R_B = 338e3;
    float R_A = 3.65e3;
    float td = 0;
    float C_RH = (1/freq - 2*td) / (LN2 * (R_A + 2*R_B));
    // freq/T compensation ...
    return C_RH;
}

float cRHc55_RH(float cRHc55) {  // C_RH / C_55
// U.P.S.I.
// C_RH/C_55 = 0.8955 + 0.002*RH , T=20C
// C_RH = C_RH(RH,T) , RH = RH(C_RH,T)
// C_RH/C_55 approx.eq. count_RH/count_ref
    float TH = get_Tntc2(0);
    float Tc = get_Temp(0);
    float rh = (cRHc55-0.8955)/0.002; // UPSI linear transfer function
    // temperature compensation
    float T0 = 0.0, T1 = -30.0; // T/C
    float T = Tc; // TH, TH-Tc (sensorT - T)
    if (T < T0) rh += T0 - T/5.5;        // approx/empirical
    if (T < T1) rh *= 1.0 + (T1-T)/75.0; // approx/empirical
    if (rh < 0.0) rh = 0.0;
    if (rh > 100.0) rh = 100.0;
    return rh;
}

float get_RHc(int csOK) { // experimental/raw, errors~10%
    float Tc = get_Temp(0);
    float count_ref = get_count_55(); // CalRef 55%RH , T=20C ?
    float count_RH = get_count_RH();
    float C_55 = get_C_RH(get_TLC555freq(count_ref), 20.0); // CalRef 55%RH , T=20C ?
    float C_RH = get_C_RH(get_TLC555freq(count_RH), Tc); // Tc == T_555 ?
    float  cRHc55 = C_RH / C_55;
    return cRHc55_RH(cRHc55);
}

float get_RH(int csOK) { // experimental/raw, errors~10%
    //ui32_t TBCREF_1000 = frame_bytes[0x32] | (frame_bytes[0x33]<<8) | (frame_bytes[0x34]<<16); // CalRef 55%RH , T=20C ?
    //ui32_t TBCCR1_1000 = frame_bytes[0x35] | (frame_bytes[0x36]<<8) | (frame_bytes[0x37]<<16); // FrqCnt TLC555
    //float  cRHc55 = TBCCR1_1000 / (float)TBCREF_1000; // CalRef 55%RH , T=20C ?
    float  cRHc55 = get_count_RH() / get_count_55(); // CalRef 55%RH , T=20C ?
    return cRHc55_RH(cRHc55);
}

/* -------------------------------------------------------------------------- */

int print_pos(int csOK) {
    int err;

    err = 0;
    err |= get_GPSweek();
    err |= get_GPStime();
    err |= get_GPSlat();
    err |= get_GPSlon();
    err |= get_GPSalt();

    if (!err) {

        Gps2Date(datum.week, datum.gpssec, &datum.jahr, &datum.monat, &datum.tag);

        if (option_color) {
            fprintf(stdout, col_TXT);
            fprintf(stdout, " (W "col_GPSweek"%d"col_TXT") ", datum.week);
            fprintf(stdout, col_GPSTOW"%s"col_TXT" ", weekday[datum.wday]);
            fprintf(stdout, col_GPSdate"%04d-%02d-%02d"col_TXT" ("col_GPSTOW"%02d:%02d:%02d"col_TXT") ",
                    datum.jahr, datum.monat, datum.tag, datum.std, datum.min, datum.sek);
            fprintf(stdout, " lat: "col_GPSlat"%.6f"col_TXT" ", datum.lat);
            fprintf(stdout, " lon: "col_GPSlon"%.6f"col_TXT" ", datum.lon);
            fprintf(stdout, " alt: "col_GPSalt"%.2f"col_TXT" ", datum.alt);
            if (option_verbose) {
                err |= get_GPSvel();
                if (!err) {
                    //if (option_verbose == 2) fprintf(stdout, "  "col_GPSvel"(%.1f , %.1f : %.1f)"col_TXT" ", datum.vx, datum.vy, datum.vD2);
                    fprintf(stdout, "  vH: "col_GPSvel"%.1f"col_TXT"  D: "col_GPSvel"%.1f"col_TXT"  vV: "col_GPSvel"%.1f"col_TXT" ", datum.vH, datum.vD, datum.vV);
                }
                if (option_verbose >= 2) {
                    get_SN();
                    fprintf(stdout, "  SN: "col_SN"%s"col_TXT, datum.SN);
                }
                if (option_verbose >= 2) {
                    fprintf(stdout, "  # ");
                    if (csOK) fprintf(stdout, " "col_CSok"[OK]"col_TXT);
                    else      fprintf(stdout, " "col_CSno"[NO]"col_TXT);
                }
            }
            if (option_ptu) {
                float t = get_Temp(csOK);
                float rh = get_RH(csOK);
                fprintf(stdout, "  ");
                if (t > -270.0) fprintf(stdout, "T=%.1fC ", t);
                if (option_verbose >= 3) { if (rh > -0.5) fprintf(stdout, "_RH=%.0f%% ", rh); }
                if (option_verbose >= 3) {
                    float t2 = get_Tntc2(csOK);
                    float fq555 = get_TLC555freq(get_count_RH());
                    if (t2 > -270.0) fprintf(stdout, " (T2:%.1fC) (%.3fkHz) ", t2, fq555/1e3);
                    fprintf(stdout, "(cRH=%.1f%%) ", get_RHc(csOK));
                }
            }
            fprintf(stdout, ANSI_COLOR_RESET"");
        }
        else {
            fprintf(stdout, " (W %d) ", datum.week);
            fprintf(stdout, "%s ", weekday[datum.wday]);
            fprintf(stdout, "%04d-%02d-%02d (%02d:%02d:%02d) ",
                    datum.jahr, datum.monat, datum.tag, datum.std, datum.min, datum.sek);
            fprintf(stdout, " lat: %.6f ", datum.lat);
            fprintf(stdout, " lon: %.6f ", datum.lon);
            fprintf(stdout, " alt: %.2f ", datum.alt);
            if (option_verbose) {
                err |= get_GPSvel();
                if (!err) {
                    //if (option_verbose == 2) fprintf(stdout, "  (%.1f , %.1f : %.1f) ", datum.vx, datum.vy, datum.vD2);
                    fprintf(stdout, "  vH: %.1f  D: %.1f  vV: %.1f ", datum.vH, datum.vD, datum.vV);
                }
                if (option_verbose >= 2) {
                    get_SN();
                    fprintf(stdout, "  SN: %s", datum.SN);
                }
                if (option_verbose >= 2) {
                    fprintf(stdout, "  # ");
                    if (csOK) fprintf(stdout, " [OK]"); else fprintf(stdout, " [NO]");
                }
            }
            if (option_ptu) {
                float t = get_Temp(csOK);
                float rh = get_RH(csOK);
                fprintf(stdout, "  ");
                if (t > -270.0) fprintf(stdout, "T=%.1fC ", t);
                if (option_verbose >= 3) { if (rh > -0.5) fprintf(stdout, "_RH=%.0f%% ", rh); }
                if (option_verbose >= 3) {
                    float t2 = get_Tntc2(csOK);
                    float fq555 = get_TLC555freq(get_count_RH());
                    if (t2 > -270.0) fprintf(stdout, " (T2:%.1fC) (%.3fkHz) ", t2, fq555/1e3);
                    fprintf(stdout, "(cRH=%.1f%%) ", get_RHc(csOK));
                }
            }
        }
        fprintf(stdout, "\n");

        if (csOK && option_sat) {
            int i;
            fprintf(stdout, " %2d", frame_bytes[pos_GPSweek-2]); // GPS-sats
            fprintf(stdout, " : ");
            for (i = 0; i < frame_bytes[pos_GPSweek-2]; i++) {   // PRN
                fprintf(stdout, " %2d", frame_bytes[pos_GPSweek+2+i]&0x3F);
            }
            fprintf(stdout, "\n");
        }

    }

    return err;
}

void print_frame(int pos) {
    int i;
    ui8_t byte;
    int cs1, cs2;
    int flen = stdFLEN; // stdFLEN=0x64, auxFLEN=0x76

    if (option_b < 2) {
        dpsk_bpm(frame_rawbits, frame_bits, RAWBITFRAME_LEN+RAWBITAUX_LEN);
    }
    bits2bytes(frame_bits, frame_bytes);
    flen = frame_bytes[0];
    if (flen == stdFLEN) auxlen = 0;
    else {
        auxlen = flen - stdFLEN;
        if (auxlen < 0 || auxlen > AUX_LEN) auxlen = 0;
    }

    cs1 = (frame_bytes[pos_Check+auxlen] << 8) | frame_bytes[pos_Check+auxlen+1];
    cs2 = checkM10(frame_bytes, pos_Check+auxlen);

    if (option_raw) {

        if (option_color  &&  frame_bytes[1] != 0x49) {
            fprintf(stdout, col_FRTXT);
            for (i = 0; i < FRAME_LEN+auxlen; i++) {
                byte = frame_bytes[i];
                if ((i >= pos_GPSTOW)   &&  (i < pos_GPSTOW+4))   fprintf(stdout, col_GPSTOW);
                if ((i >= pos_GPSlat)   &&  (i < pos_GPSlat+4))   fprintf(stdout, col_GPSlat);
                if ((i >= pos_GPSlon)   &&  (i < pos_GPSlon+4))   fprintf(stdout, col_GPSlon);
                if ((i >= pos_GPSalt)   &&  (i < pos_GPSalt+4))   fprintf(stdout, col_GPSalt);
                if ((i >= pos_GPSweek)  &&  (i < pos_GPSweek+2))  fprintf(stdout, col_GPSweek);
                if ((i >= pos_GPSvE)    &&  (i < pos_GPSvE+6))    fprintf(stdout, col_GPSvel);
                if ((i >= pos_SN)       &&  (i < pos_SN+5))       fprintf(stdout, col_SN);
                if ((i >= pos_Check+auxlen)  &&  (i < pos_Check+auxlen+2))  fprintf(stdout, col_Check);
                fprintf(stdout, "%02x", byte);
                fprintf(stdout, col_FRTXT);
            }
            if (option_verbose) {
                fprintf(stdout, " # "col_Check"%04x"col_FRTXT, cs2);
                if (cs1 == cs2) fprintf(stdout, " "col_CSok"[OK]"col_TXT);
                else            fprintf(stdout, " "col_CSno"[NO]"col_TXT);
            }
            fprintf(stdout, ANSI_COLOR_RESET"\n");
        }
        else {
            for (i = 0; i < FRAME_LEN+auxlen; i++) {
                byte = frame_bytes[i];
                fprintf(stdout, "%02x", byte);
            }
            if (option_verbose) {
                fprintf(stdout, " # %04x", cs2);
                if (cs1 == cs2) fprintf(stdout, " [OK]"); else fprintf(stdout, " [NO]");
            }
            fprintf(stdout, "\n");
        }

    }
    else if (frame_bytes[1] == 0x49) {
        if (option_verbose == 3) {
            for (i = 0; i < FRAME_LEN+auxlen; i++) {
                byte = frame_bytes[i];
                fprintf(stdout, "%02x", byte);
            }
            fprintf(stdout, "\n");
        }
    }
    else print_pos(cs1 == cs2);

}


int main(int argc, char **argv) {

    FILE *fp;
    char *fpname;
    int i, len;
    int bit, bit0;
    int pos;
    int header_found = 0;


#ifdef CYGWIN
    _setmode(fileno(stdin), _O_BINARY);  // _setmode(_fileno(stdin), _O_BINARY);
#endif
    setbuf(stdout, NULL);

    fpname = argv[0];
    ++argv;
    while ((*argv) && (!wavloaded)) {
        if      ( (strcmp(*argv, "-h") == 0) || (strcmp(*argv, "--help") == 0) ) {
            fprintf(stderr, "%s [options] audio.wav\n", fpname);
            fprintf(stderr, "  options:\n");
            //fprintf(stderr, "       -v, --verbose\n");
            fprintf(stderr, "       -r, --raw\n");
            fprintf(stderr, "       -c, --color\n");
            //fprintf(stderr, "       -o, --offset\n");
            return 0;
        }
        else if ( (strcmp(*argv, "-v") == 0) || (strcmp(*argv, "--verbose") == 0) ) {
            option_verbose = 1;
        }
        else if ( (strcmp(*argv, "-vv" ) == 0) ) option_verbose = 2;
        else if ( (strcmp(*argv, "-vvv") == 0) ) option_verbose = 3;
        else if ( (strcmp(*argv, "-r") == 0) || (strcmp(*argv, "--raw") == 0) ) {
            option_raw = 1;
        }
        else if ( (strcmp(*argv, "-i") == 0) || (strcmp(*argv, "--invert") == 0) ) {
            option_inv = 1;  // nicht noetig
        }
        else if ( (strcmp(*argv, "-c") == 0) || (strcmp(*argv, "--color") == 0) ) {
            option_color = 1;
        }
        else if   (strcmp(*argv, "--res") == 0) { option_res = 1; }
        else if ( (strcmp(*argv, "--avg") == 0) ) {
            option_avg = 1;
        }
        else if   (strcmp(*argv, "-b" ) == 0) { option_b = 1; }
        else if   (strcmp(*argv, "-b2") == 0) { option_b = 2; }
        else if ( (strcmp(*argv, "--ptu") == 0) ) {
            option_ptu = 1;
        }
        else if ( (strcmp(*argv, "--sat") == 0) ) {
            option_sat = 1;
        }
        else if   (strcmp(*argv, "--ch2") == 0) { wav_channel = 1; }  // right channel (default: 0=left)
        else {
            fp = fopen(*argv, "rb");
            if (fp == NULL) {
                fprintf(stderr, "%s konnte nicht geoeffnet werden\n", *argv);
                return -1;
            }
            wavloaded = 1;
        }
        ++argv;
    }
    if (!wavloaded) fp = stdin;


    i = read_wav_header(fp);
    if (i) {
        fclose(fp);
        return -1;
    }


    pos = FRAMESTART;

    while (!read_bits_fsk(fp, &bit, &len)) {

        if (len == 0) { // reset_frame();
            if (pos > (pos_GPSweek+2)*2*BITS) {
                for (i = pos; i < RAWBITFRAME_LEN+RAWBITAUX_LEN; i++) frame_rawbits[i] = 0x30 + 0;
                print_frame(pos);//byte_count
                header_found = 0;
                pos = FRAMESTART;
            }
            //inc_bufpos();
            //buf[bufpos] = 'x';
            continue;   // ...
        }

        for (i = 0; i < len; i++) {

            inc_bufpos();
            buf[bufpos] = 0x30 + bit;  // Ascii

            if (!header_found) {
                header_found = compare2();
            }
            else {
                frame_rawbits[pos] = 0x30 + bit;  // Ascii
                pos++;

                if (pos == RAWBITFRAME_LEN+RAWBITAUX_LEN) {
                    frame_rawbits[pos] = '\0';
                    print_frame(pos);//FRAME_LEN
                    header_found = 0;
                    pos = FRAMESTART;
                }
            }

        }
        if (header_found && option_b==1) {
            bitstart = 1;

            while ( pos < RAWBITFRAME_LEN+RAWBITAUX_LEN ) {
                if (read_rawbit(fp, &bit) == EOF) break;
                frame_rawbits[pos] = 0x30 + bit;
                pos++;
            }
            frame_rawbits[pos] = '\0';
            print_frame(pos);
            header_found = 0;
            pos = FRAMESTART;
        }
        if (header_found && option_b>=2) {
            bitstart = 1;
            bit0 = 0;

            if (pos%2) {
                if (read_rawbit(fp, &bit) == EOF) break;
                    frame_rawbits[pos] = 0x30 + bit;
                    pos++;
            }

            bit0 = dpsk_bpm(frame_rawbits, frame_bits, pos);
            pos /= 2;

            while ( pos < BITFRAME_LEN+BITAUX_LEN ) {
                if (read_rawbit2(fp, &bit) == EOF) break;
                frame_bits[pos] = 0x31 ^ (bit0 ^ bit);
                pos++;
                bit0 = bit;
            }
            frame_bits[pos] = '\0';
            print_frame(pos);
            header_found = 0;
            pos = FRAMESTART;
        }
    }

    fprintf(stdout, "\n");

    fclose(fp);

    return 0;
}

