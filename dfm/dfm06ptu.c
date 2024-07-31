
/*
 *
 * DFM-06 und DFM-09 haben unterschiedliche Polaritaet bzw. Manchester-Varianten
 * (Polaritaet PS-15 wie DFM-06)
 * DFM-06 hat Kanaele 0..6 (anfangs nur 0..5)
 * DFM-09 hat Kanaele 0..A
 * Ausnahme: erste DFM-09-Versionen senden wie DFM-06
 * PS-15  hat Kanaele 0..7
 *
 * Optionen:
 *   -v, -vv  verbose/velocity, SN
 *   -r, -R   raw frames
 *   -i       invertiert Signal (DFM-06 / DFM-09)
 *   -b,-b2   alternative Demodulation (symbol/bit integration)
 *   --ecc    Hamming Error Correction
 *   --ptu    PTU: temperature
 *   --auto   detect polarity/SN
 */

#include <stdio.h>
#include <string.h>

#include <math.h>
#include <stdlib.h>

#ifdef CYGWIN
  #include <fcntl.h>  // cygwin: _setmode()
  #include <io.h>
#endif


typedef unsigned char ui8_t;
typedef unsigned int ui32_t;


typedef struct {
    int frnr;
    int sonde_typ;
    ui32_t SN6;
    ui32_t SN;
    int week; int gpssec;
    int jahr; int monat; int tag;
    int std; int min; float sek;
    double lat; double lon; double alt;
    double dir; double horiV; double vertV;
    float meas24[5];
    float status[2];
    char sonde_id[16]; // "ID__:xxxxxxxx\0\0"
} gpx_t;

gpx_t gpx;

char dat_str[9][13+1];


int option_verbose = 0,  // ausfuehrliche Anzeige
    option_raw = 0,      // rohe Frames
    option_inv = 0,      // invertiert Signal
    option_auto = 0,
    option_avg = 0,      // moving average
    option_b = 0,
    option_ecc = 0,
    option_ptu = 0,
    wavloaded = 0;
int wav_channel = 0;     // audio channel: left

int ptu_out = 0;

int start = 0;

/* -------------------------------------------------------------------------- */

// option_b: exakte Baudrate wichtig!
// eventuell in header ermittelbar
#define BAUD_RATE   2500

int sample_rate = 0, bits_sample = 0, channels = 0;
float samples_per_bit = 0;

int findstr(char *buff, char *str, int pos) {
    int i;
    for (i = 0; i < 4; i++) {
        if (buff[(pos+i)%4] != str[i]) break;
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
    int n;
    float l;

    n = 0;
    do {
        sample = read_signed_sample(fp);

        if (sample == EOF_INT) return EOF;

        par_alt = par;
        par =  (sample >= 0) ? 1 : -1;    // 8bit: 0..127,128..255 (-128..-1,0..127)
        n++;
    } while (par*par_alt > 0);

    l = (float)n / samples_per_bit;

    *len = (int)(l+0.5);

    if (!option_inv) *bit = (1+par_alt)/2;  // oben 1, unten -1
    else             *bit = (1-par_alt)/2;  // sdr#<rev1381?, invers: unten 1, oben -1
// *bit = (1+inv*par_alt)/2; // ausser inv=0

    /* Y-offset ? */

    return 0;
}

int bitstart = 0;
double bitgrenze = 0;
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

float *wc = NULL;

int read_rawbit3(FILE *fp, int *bit) {
    int sample;
    int n;
    float sum;

    sum = 0;
    n = 0;

    if (bitstart) {
        n = 1;         // sample*wc[0] ?
        scount = 1;    // (sample_count overflow/wrap-around)
        bitgrenze = 0; // d.h. bitgrenze = sample_count-1 (?)
        bitstart = 0;
    }

    bitgrenze += 2*samples_per_bit;
    do {
        sample = read_signed_sample(fp);
        if (sample == EOF_INT) return EOF;
        //sample_count++; // in read_signed_sample()
        //par =  (sample >= 0) ? 1 : -1;    // 8bit: 0..127,128..255 (-128..-1,0..127)
        sum += sample*wc[n];
        n++;
        scount++;
    } while (scount < bitgrenze);  // n < samples_per_bit

    if (sum >= 0) *bit = 1;
    else          *bit = 0;

    if (option_inv) *bit ^= 1;

    return 0;
}


typedef struct {
    ui8_t hb;
    float sb;
} hsbit_t;

int soft_read_rawbit2(FILE *fp, hsbit_t *shb) {
    int sample;
    int sum;
    ui8_t bit = 0;

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

    if (option_inv) sum = -sum;  // sum=0 bleibt bit=1
                                 // jedoch sb und hb werden zusammen invertiert
    if (sum >= 0) bit = 1;
    else          bit = 0;

    shb->hb = bit;
    shb->sb = sum;

    return 0;
}

/* -------------------------------------------------------------------------- */

//#define BITS (2*8)  // 16
#define HEADLEN 32  // HEADLEN+HEADOFS=32 <= strlen(header)
#define HEADOFS  0
char header[] = "01100101011001101010010110101010";

char buf[HEADLEN+1] = "xxxxxxxxxx\0";
int bufpos = -1;


#define BITFRAME_LEN     280
#define RAWBITFRAME_LEN (BITFRAME_LEN*2)
#define FRAMESTART      (HEADOFS+HEADLEN)


char frame_rawbits[RAWBITFRAME_LEN+8] = "01100101011001101010010110101010"; //->"0100010111001111";
char frame_bits[BITFRAME_LEN+4];

hsbit_t frm[BITFRAME_LEN+4];


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

    if (option_auto) {
    i = 0;
    j = bufpos;
    while (i < HEADLEN) {
        if (j < 0) j = HEADLEN-1;
        if (buf[j] != cb_inv(header[HEADOFS+HEADLEN-1-i])) break;
        j--;
        i++;
    }
    if (i == HEADLEN) return -1;
    }

    return 0;

}

// manchester1 1->10,0->01: 1.bit
// manchester2 0->10,1->01: 2.bit
void manchester1(char* frame_rawbits, char *frame_bits, int pos) {
    int i, c, out, buf;
    char bit, bits[2];
    c = 0;

    for (i = 0; i < pos/2; i++) {  // -16
        bits[0] = frame_rawbits[2*i];
        bits[1] = frame_rawbits[2*i+1];

        if ((bits[0] == '0') && (bits[1] == '1')) { bit = '0'; out = 1; }
        else
        if ((bits[0] == '1') && (bits[1] == '0')) { bit = '1'; out = 1; }
        else { //
            if (buf == 0) { c = !c; out = 0; buf = 1; }
            else { bit = 'x'; out = 1; buf = 0; }
        }
        if (out) frame_bits[i] = bit;
    }
}

/* -------------------------------------------------------------------------- */


#define B 8 // codeword: 8 bit
#define S 4 // davon 4 bit data

#define HEAD 0        //  16 bit
#define CONF (16+0)   //  56 bit
#define DAT1 (16+56)  // 104 bit
#define DAT2 (16+160) // 104 bit
               // frame: 280 bit

ui8_t G[8][4] =  // Generator
             {{ 1, 0, 0, 0},
              { 0, 1, 0, 0},
              { 0, 0, 1, 0},
              { 0, 0, 0, 1},
              { 0, 1, 1, 1},
              { 1, 0, 1, 1},
              { 1, 1, 0, 1},
              { 1, 1, 1, 0}};

ui8_t H[4][8] =  // Parity-Check
             {{ 0, 1, 1, 1, 1, 0, 0, 0},
              { 1, 0, 1, 1, 0, 1, 0, 0},
              { 1, 1, 0, 1, 0, 0, 1, 0},
              { 1, 1, 1, 0, 0, 0, 0, 1}};
ui8_t He[8] = { 0x7, 0xB, 0xD, 0xE, 0x8, 0x4, 0x2, 0x1}; // Spalten von H:
                                                         // 1-bit-error-Syndrome
ui8_t hamming_conf[ 7*B];  //  7*8=56
ui8_t hamming_dat1[13*B];  // 13*8=104
ui8_t hamming_dat2[13*B];

ui8_t block_conf[ 7*S];  //  7*4=28
ui8_t block_dat1[13*S];  // 13*4=52
ui8_t block_dat2[13*S];

hsbit_t soft_hamming_conf[ 7*B];  //  7*8=56
hsbit_t soft_hamming_dat1[13*B];  // 13*8=104
hsbit_t soft_hamming_dat2[13*B];

ui8_t codewords[16][8];

int nib4bits(ui8_t nib, ui8_t *bits) { // big endian
    int j;

    nib &= 0xF;
    for (j = 0; j < 4; j++) {
        bits[j] = (nib>>(3-j)) & 0x1;
    }
    return 0;
}

int gencode(ui8_t msg[4], ui8_t code[8]) {
    int i, j;                  // Gm=c
    for (i = 0; i < 8; i++) {
        code[i] = 0;
        for (j = 0; j < 4; j++) {
            code[i] ^= G[i][j] & msg[j];
        }
    }
    return 0;
}

ui32_t bits2val(ui8_t *bits, int len) { // big endian
    int j;
    ui32_t val;
    if ((len < 0) || (len > 32)) return -1;
    val = 0;
    for (j = 0; j < len; j++) {
        val |= (bits[j] << (len-1-j));
    }
    return val;
}

void deinterleave(char *str, int L, ui8_t *block) {
    int i, j;
    for (j = 0; j < B; j++) {  // L = 7, 13
        for (i = 0; i < L; i++) {
            if (str[L*j+i] >= 0x30 && str[L*j+i] <= 0x31) {
                block[B*i+j] = str[L*j+i] - 0x30; // ASCII -> bit
            }
        }
    }
}

void soft_deinterleave(hsbit_t *str, int L, hsbit_t *block) {
    int i, j;
    for (j = 0; j < B; j++) {  // L = 7, 13
        for (i = 0; i < L; i++) {
            block[B*i+j] = str[L*j+i];
        }
    }
}

int check(ui8_t code[8]) {
    int i, j;               // Bei Demodulierung durch Nulldurchgaenge, wenn durch Fehler ausser Takt,
    ui32_t synval = 0;      // verschieben sich die bits. Fuer Hamming-Decode waere es besser,
    ui8_t syndrom[4];       // sync zu Beginn mit Header und dann Takt beibehalten fuer decision.
    int ret=0;

    for (i = 0; i < 4; i++) { // S = 4
        syndrom[i] = 0;
        for (j = 0; j < 8; j++) { // B = 8
            syndrom[i] ^= H[i][j] & code[j];
        }
    }
    synval = bits2val(syndrom, 4);
    if (synval) {
        ret = -1;
        for (j = 0; j < 8; j++) {   // 1-bit-error
            if (synval == He[j]) {  // reicht auf databits zu pruefen, d.h.
                ret = j+1;          // (systematischer Code) He[0..3]
                break;
            }
        }
    }
    else ret = 0;
    if (ret > 0) code[ret-1] ^= 0x1;

    return ret;
}

int hamming(ui8_t *ham, int L, ui8_t *sym) {
    int i, j;
    int ret = 0;               // L = 7, 13
    for (i = 0; i < L; i++) {  // L * 2 nibble (data+parity)
        if (option_ecc) ret |= check(ham+B*i);
        for (j = 0; j < S; j++) {  // systematic: bits 0..S-1 data
            sym[S*i+j] = ham[B*i+j];
        }
    }
    return ret;
}

int soft_check(hsbit_t code[8]) {
    int i, j;               // Bei Demodulierung durch Nulldurchgaenge, wenn durch Fehler ausser Takt,
    ui32_t synval = 0;      // verschieben sich die bits. Fuer Hamming-Decode waere es besser,
    ui8_t syndrom[4];       // sync zu Beginn mit Header und dann Takt beibehalten fuer decision.
    int ret=0;

    for (i = 0; i < 4; i++) { // S = 4
        syndrom[i] = 0;
        for (j = 0; j < 8; j++) { // B = 8
            syndrom[i] ^= H[i][j] & code[j].hb;
        }
    }
    synval = bits2val(syndrom, 4);
    if (synval) {
        ret = -1;
        for (j = 0; j < 8; j++) {   // 1-bit-error
            if (synval == He[j]) {  // reicht auf databits zu pruefen, d.h.
                ret = j+1;          // (systematischer Code) He[0..3]
                break;
            }
        }
    }
    else ret = 0; // d=0: valid codeword

    if (ret > 0) code[ret-1].hb ^= 0x1;    // d=1: 1-bit-error
    else if (ret < 0 && option_ecc == 2) { // d=2: 2-bit-error: soft decision
        int n;
        int count = 0;
        int maxn = -1;
        int d = 0;
        float sum = 0.0;
        float maxsum = 0.0;
        for (n = 0; n < 16; n++) {
            d = 0;
            for (i = 0; i < 8; i++) { // d(a,b) = sum_i a[i]^b[i]
                if (code[i].hb != codewords[n][i]) d++;
            }
            if (d == 2) { // dist=2
                count++;
                // sum softbits
                // correlation: - interleaving
                //              + no pulse-shaping -> sum
                //
                sum = 0.0;
                for (i = 0; i < 8; i++) {
                    sum += (2*codewords[n][i]-1) * code[i].sb;
                }
                if (sum >= maxsum) {
                    maxsum = sum;
                    maxn = n;
                }
            }
        }
        if (maxn >= 0) {
            for (i = 0; i < 8; i++) {
                if (code[i].hb = codewords[maxn][i]);
            }
            //ret = 0x100;
        }
    }

    return ret;
}

int soft_hamming(hsbit_t *ham, int L, ui8_t *sym) {
    int i, j;
    int ret = 0;               // L = 7, 13
    for (i = 0; i < L; i++) {  // L * 2 nibble (data+parity)
        if (option_ecc) ret |= soft_check(ham+B*i);
        for (j = 0; j < S; j++) {  // systematic: bits 0..S-1 data
            sym[S*i+j] = ham[B*i+j].hb;
        }
    }
    return ret;
}

char nib2chr(ui8_t nib) {
    char c = '_';
    if (nib < 0x10) {
        if (nib < 0xA)  c = 0x30 + nib;
        else            c = 0x41 + nib-0xA;
    }
    return c;
}

int dat_out(ui8_t *dat_bits) {
    int i, ret = 0;
    static int fr_id;
    // int jahr = 0, monat = 0, tag = 0, std = 0, min = 0;
    int frnr = 0;
    int msek = 0;
    int lat = 0, lon = 0, alt = 0;
    int nib;
    int dvv;  // signed/unsigned 16bit

    fr_id = bits2val(dat_bits+48, 4);


    if (fr_id >= 0 && fr_id <= 8) {
        for (i = 0; i < 13; i++) {
            nib = bits2val(dat_bits+4*i, 4);
            dat_str[fr_id][i] = nib2chr(nib);
        }
        dat_str[fr_id][13] = '\0';
    }

    if (fr_id == 0) {
        start = 1;
        frnr = bits2val(dat_bits+24, 8);
        gpx.frnr = frnr;
    }

    if (fr_id == 1) {
        // 00..31: ? GPS-Sats in Sicht?
        msek = bits2val(dat_bits+32, 16);  // UTC (= GPS - 18sec  ab 1.1.2017)
        gpx.sek = msek/1000.0;
    }

    if (fr_id == 2) {
        lat = bits2val(dat_bits, 32);
        gpx.lat = lat/1e7;
        dvv = (short)bits2val(dat_bits+32, 16);  // (short)? zusammen mit dir sollte unsigned sein
        gpx.horiV = dvv/1e2;
    }

    if (fr_id == 3) {
        lon = bits2val(dat_bits, 32);
        gpx.lon = lon/1e7;
        dvv = bits2val(dat_bits+32, 16) & 0xFFFF;  // unsigned
        gpx.dir = dvv/1e2;
    }

    if (fr_id == 4) {
        alt = bits2val(dat_bits, 32);
        gpx.alt = alt/1e2;
        dvv = (short)bits2val(dat_bits+32, 16);  // signed
        gpx.vertV = dvv/1e2;
    }

    if (fr_id == 5) {
    }

    if (fr_id == 6) { // sat data
    }

    if (fr_id == 7) { // sat data
    }

    if (fr_id == 8) {
        gpx.jahr  = bits2val(dat_bits,   12);
        gpx.monat = bits2val(dat_bits+12, 4);
        gpx.tag   = bits2val(dat_bits+16, 5);
        gpx.std   = bits2val(dat_bits+21, 5);
        gpx.min   = bits2val(dat_bits+26, 6);
    }

    ret = fr_id;
    return ret;
}

// DFM-06 (NXP8)
float fl20(int d) {  // float20
    int val, p;
    float f;
    p = (d>>16) & 0xF;
    val = d & 0xFFFF;
    f = val/(float)(1<<p);
    return  f;
}
/*
float flo20(int d) {
    int m, e;
    float f1, f;
    m = d & 0xFFFF;
    e = (d >> 16) & 0xF;
    f =  m / pow(2,e);
    return  f;
}
*/

// DFM-09 (STM32)
float fl24(int d) {  // float24
    int val, p;
    float f;
    p = (d>>20) & 0xF;
    val = d & 0xFFFFF;
    f = val/(float)(1<<p);
    return  f;
}

// temperature approximation
float get_Temp(float *meas) { // meas[0..4]
// NTC-Thermistor EPCOS B57540G0502
// R/T No 8402, R25=Ro=5k
// B0/100=3450
// 1/T = 1/To + 1/B log(r) , r=R/Ro
// GRAW calibration data -80C..+40C on EEPROM ?
// meas0 = g*(R + Rs)
// meas3 = g*Rs , Rs: dfm6:10k, dfm9:20k
// meas4 = g*Rf , Rf=220k
    float B0 = 3260.0;       // B/Kelvin, fit -55C..+40C
    float T0 = 25 + 273.15;  // t0=25C
    float R0 = 5.0e3;        // R0=R25=5k
    float Rf = 220e3;        // Rf = 220k
    float g = meas[4]/Rf;
    float R = (meas[0]-meas[3]) / g; // meas[0,3,4] > 0 ?
    float T = 0;                     // T/Kelvin
    if (meas[0]*meas[3]*meas[4] == 0) R = 0;
    if (R > 0)  T = 1/(1/T0 + 1/B0 * log(R/R0));
    return  T - 273.15; // Celsius
//  DFM-06: meas20 * 16 = meas24
//      -> (meas24[0]-meas24[3])/meas24[4]=(meas20[0]-meas20[3])/meas20[4]
}
float get_Temp2(float *meas) { // meas[0..4]
// NTC-Thermistor EPCOS B57540G0502
// R/T No 8402, R25=Ro=5k
// B0/100=3450
// 1/T = 1/To + 1/B log(r) , r=R/Ro
// GRAW calibration data -80C..+40C on EEPROM ?
// meas0 = g*(R+Rs)+ofs
// meas3 = g*Rs+ofs , Rs: dfm6:10k, dfm9:20k
// meas4 = g*Rf+ofs , Rf=220k
    float f  = meas[0],
          f1 = meas[3],
          f2 = meas[4];
    float B0 = 3260.0;      // B/Kelvin, fit -55C..+40C
    float T0 = 25 + 273.15; // t0=25C
    float R0 = 5.0e3;       // R0=R25=5k
    float Rf2 = 220e3;      // Rf2 = Rf = 220k
    float g_o = f2/Rf2;     // approx gain
    float Rs_o = f1/g_o;    // = Rf2 * f1/f2;
    float Rf1 = Rs_o;       // Rf1 = Rs: dfm6:10k, dfm9:20k
    float g = g_o;          // gain
    float Rb = 0.0;         // offset
    float R = 0;            // thermistor
    float T = 0;            // T/Kelvin

    if       ( 8e3 < Rs_o && Rs_o < 12e3) Rf1 = 10e3;  // dfm6
    else if  (18e3 < Rs_o && Rs_o < 22e3) Rf1 = 20e3;  // dfm9
    g = (f2 - f1) / (Rf2 - Rf1);
    Rb = (f1*Rf2-f2*Rf1)/(f2-f1); // ofs/g

    R = (f-f1)/g;                    // meas[0,3,4] > 0 ?
    if (R > 0)  T = 1/(1/T0 + 1/B0 * log(R/R0));

    if (option_ptu && ptu_out && option_verbose == 2) {
        printf("  (Rso: %.1f , Rb: %.1f)", Rs_o/1e3, Rb/1e3);
    }

    return  T - 273.15;
//  DFM-06: meas20 * 16 = meas24
}
float get_Temp4(float *meas) { // meas[0..4]
// NTC-Thermistor EPCOS B57540G0502
// [  T/C  ,   R/R25   , alpha ] :
// [ -55.0 ,  51.991   ,   6.4 ]
// [ -50.0 ,  37.989   ,   6.2 ]
// [ -45.0 ,  28.07    ,   5.9 ]
// [ -40.0 ,  20.96    ,   5.7 ]
// [ -35.0 ,  15.809   ,   5.5 ]
// [ -30.0 ,  12.037   ,   5.4 ]
// [ -25.0 ,   9.2484  ,   5.2 ]
// [ -20.0 ,   7.1668  ,   5.0 ]
// [ -15.0 ,   5.5993  ,   4.9 ]
// [ -10.0 ,   4.4087  ,   4.7 ]
// [  -5.0 ,   3.4971  ,   4.6 ]
// [   0.0 ,   2.7936  ,   4.4 ]
// [   5.0 ,   2.2468  ,   4.3 ]
// [  10.0 ,   1.8187  ,   4.2 ]
// [  15.0 ,   1.4813  ,   4.0 ]
// [  20.0 ,   1.2136  ,   3.9 ]
// [  25.0 ,   1.0000  ,   3.8 ]
// [  30.0 ,   0.82845 ,   3.7 ]
// [  35.0 ,   0.68991 ,   3.6 ]
// [  40.0 ,   0.57742 ,   3.5 ]
// -> Steinhart–Hart coefficients (polyfit):
    float p0 = 1.09698417e-03,
          p1 = 2.39564629e-04,
          p2 = 2.48821437e-06,
          p3 = 5.84354921e-08;
// T/K = 1/( p0 + p1*ln(R) + p2*ln(R)^2 + p3*ln(R)^3 )
    float Rf = 220e3;    // Rf = 220k
    float g = meas[4]/Rf;
    float R = (meas[0]-meas[3]) / g; // meas[0,3,4] > 0 ?
    float T = 0; // T/Kelvin
    if (R > 0)  T = 1/( p0 + p1*log(R) + p2*log(R)*log(R) + p3*log(R)*log(R)*log(R) );
    return  T - 273.15; // Celsius
//  DFM-06: meas20 * 16 = meas24
//      -> (meas24[0]-meas24[3])/meas24[4]=(meas20[0]-meas20[3])/meas20[4]
}


#define SNbit 0x0100
int conf_out(ui8_t *conf_bits) {
    int ret = 0;
    int val;
    ui8_t conf_id;
    ui8_t hl;
    ui32_t SN6, SN;
    static int chAbit, chA[2];
    static int chCbit, chC[2];
    static int chDbit, chD[2];
    static int ch7bit, ch7[2];
    static ui32_t SN_A, SN_C, SN_D, SN_7;
    static ui8_t max_ch;
    static ui8_t nul_ch;
    static ui8_t sn2_ch, sn_ch;
    static ui32_t SN_X;
    static int chXbit, chX[2];
    static ui8_t dfm6typ;

    conf_id = bits2val(conf_bits, 4);

    if (conf_id > 4 && bits2val(conf_bits+8, 4*5) == 0) nul_ch = bits2val(conf_bits, 8);

    dfm6typ = ((nul_ch & 0xF0)==0x50) && (nul_ch & 0x0F);
    if (dfm6typ) ptu_out = 6;
    if (dfm6typ  && (gpx.sonde_typ & 0xF) > 6)
    {   // reset if 0x5A, 0x5B (DFM-06)
        gpx.sonde_typ = 0;
        max_ch = conf_id;
    }

    if (conf_id > 4 && conf_id > max_ch) max_ch = conf_id; // mind. 5 Kanaele // reset? lower 0xsCaaaab?

    if (conf_id > 4 && conf_id == (nul_ch>>4)+1)
    {
        sn2_ch = bits2val(conf_bits, 8);

        if (option_auto)
        {
            sn_ch = ((sn2_ch>>4) & 0xF);
            if (conf_id == sn_ch)
            {
                if ( (nul_ch & 0x58) == 0x58 ) { // 0x5A, 0x5B
                    SN6 = bits2val(conf_bits+4, 4*6);   // DFM-06: Kanal 6
                    if (SN6 == gpx.SN6  &&  SN6 != 0) { // nur Nibble-Werte 0..9
                        gpx.sonde_typ = SNbit | 6;
                        ptu_out = 6;
                        sprintf(gpx.sonde_id, "ID06:%6X", gpx.SN6);
                    }
                    else { // reset
                        gpx.sonde_typ = 0;
                    }
                    gpx.SN6 = SN6;
                }
                else if (   (sn2_ch & 0xF) == 0xC    // 0xsCaaaab, s==sn_ch , s: 0xA=DFM-09 , 0xC/0xD=DFM-17?
                         || (sn2_ch & 0xF) == 0x0 )  // 0xs0aaaab, s==sn_ch , s: 0x7,0x8: pilotsonde PS-15?
                {
                    val = bits2val(conf_bits+8, 4*5);
                    hl =  (val & 1);
                    chX[hl] = (val >> 4) & 0xFFFF;
                    chXbit |= 1 << hl;
                    if (chXbit == 3) {
                        SN = (chX[0] << 16) | chX[1];
                        if ( SN == SN_X || SN_X == 0 ) {

                            gpx.sonde_typ = SNbit | sn_ch;
                            gpx.SN = SN;

                            if (sn_ch == 0xA /*&& (sn2_ch & 0xF) == 0xC*/) ptu_out = 9; else ptu_out = 0;
                            // PS-15 ? (sn2_ch & 0xF) == 0x0 :  ptu_out = 0
                            // DFM-17? (sn_ch == 0xC) ptu_out = 9 ? // test 0xD ...?

                            if ( (gpx.sonde_typ & 0xF) == 0xA) {
                                sprintf(gpx.sonde_id, "ID09:%6u", gpx.SN);
                            }
                            else {
                                sprintf(gpx.sonde_id, "ID-%1X:%6u", gpx.sonde_typ & 0xF, gpx.SN);
                            }
                        }
                        else { // reset
                            gpx.sonde_typ = 0;
                        }
                        SN_X = SN;
                        chXbit = 0;
                    }
                }
                ret = (gpx.sonde_typ & 0xF);
            }
        }
    }

    if (option_auto == 0) {

        // gibt es Kanaele > 6 (2-teilige ID)?
        // if (conf_id > 6) gpx.SN6 = 0;  // -> DFM-09,PS-15  // SNbit?
        //
        // SN/ID immer im letzten Kanal? davor xy00000-Kanal? (mind. 1)
        if ((gpx.sonde_typ & 0xF) < 7  &&  conf_id == 6) {
            SN6 = bits2val(conf_bits+4, 4*6);   // DFM-06: Kanal 6
            if (SN6 == gpx.SN6  &&  SN6 != 0) { // nur Nibble-Werte 0..9
                gpx.sonde_typ = SNbit | 6;
                ptu_out = 6;
                ret = 6;
                sprintf(gpx.sonde_id, "ID06:%6X", gpx.SN6);
            }
            else {
                gpx.sonde_typ = 0;
            }
            gpx.SN6 = SN6;
        }
        if (conf_id == 0xA) {  // 0xACxxxxy ,  DFM-09
            val = bits2val(conf_bits+8, 4*5);
            hl =  (val & 1);  // val&0xF 0,1?
            chA[hl] = (val >> 4) & 0xFFFF;
            chAbit |= 1 << hl;
            if (chAbit == 3) {  // DFM-09: Kanal A
                SN = (chA[0] << 16) | chA[1];
                if ( SN == SN_A ) {
                    gpx.sonde_typ = SNbit | 0xA;
                    gpx.SN = SN;
                    ptu_out = 9;
                    ret = 9;
                    sprintf(gpx.sonde_id, "ID09:%6u", gpx.SN);
                }
                else {
                    gpx.sonde_typ = 0;
                }
                SN_A = SN;
                chAbit = 0;
            }
        }
        if (conf_id == 0xC) {  // 0xCCxxxxy ,  DFM-17?
            val = bits2val(conf_bits+8, 4*5);
            hl =  (val & 1);
            chC[hl] = (val >> 4) & 0xFFFF;
            chCbit |= 1 << hl;
            if (chCbit == 3) {  // DFM-17? Kanal C
                SN = (chC[0] << 16) | chC[1];
                if ( SN == SN_C ) {
                    gpx.sonde_typ = SNbit | 0xC;
                    gpx.SN = SN;
                    ptu_out = 9;
                    ret = 17;
                    sprintf(gpx.sonde_id, "ID-%1X:%6u", gpx.sonde_typ & 0xF, gpx.SN);
                }
                else {
                    gpx.sonde_typ = 0;
                }
                SN_C = SN;
                chCbit = 0;
            }
        }
        if (conf_id == 0xD) {  // 0xDCxxxxy ,  DFM-17?
            val = bits2val(conf_bits+8, 4*5);
            hl =  (val & 1);
            chD[hl] = (val >> 4) & 0xFFFF;
            chDbit |= 1 << hl;
            if (chDbit == 3) {  // DFM-17? Kanal D
                SN = (chD[0] << 16) | chD[1];
                if ( SN == SN_D ) {
                    gpx.sonde_typ = SNbit | 0xD;
                    gpx.SN = SN;
                    ptu_out = 9;
                    ret = 18;
                    sprintf(gpx.sonde_id, "ID-%1X:%6u", gpx.sonde_typ & 0xF, gpx.SN);
                }
                else {
                    gpx.sonde_typ = 0;
                }
                SN_D = SN;
                chDbit = 0;
            }
        }
        if (conf_id == 0x7) {  // 0x70xxxxy ,  pilotsonde PS-15?
            val = bits2val(conf_bits+8, 4*5);
            hl =  (val & 1);
            ch7[hl] = (val >> 4) & 0xFFFF;
            ch7bit |= 1 << hl;
            if (ch7bit == 3) {  // PS-15: Kanal 7
                SN = (ch7[0] << 16) | ch7[1];
                if ( SN == SN_7 ) {
                    gpx.sonde_typ = SNbit | 0x7;
                    gpx.SN = SN;
                    ptu_out = 0;
                    ret = 15;
                    sprintf(gpx.sonde_id, "ID15:%6u", gpx.SN);
                }
                else {
                    gpx.sonde_typ = 0;
                }
                SN_7 = SN;
                ch7bit = 0;
            }
        }
    }


    if (conf_id >= 0 && conf_id <= 4) {
        val = bits2val(conf_bits+4, 4*6);
        gpx.meas24[conf_id] = fl24(val);
        // DFM-09 (STM32): 24bit 0exxxxx
        // DFM-06 (NXP8):  20bit 0exxxx0
        //   fl20(bits2val(conf_bits+4, 4*5))
        //       = fl20(exxxx)
        //       = fl24(exxxx0)/2^4
        //   meas20 * 16 = meas24
    }

    // STM32-status: Bat, MCU-Temp
    if ((gpx.sonde_typ & 0xF) == 0xA) { // DFM-09 (STM32)
        if (conf_id == 0x5) { // voltage
            val = bits2val(conf_bits+8, 4*4);
            gpx.status[0] = val/1000.0;
        }
        if (conf_id == 0x6) { // T-intern (STM32)
            val = bits2val(conf_bits+8, 4*4);
            gpx.status[1] = val/100.0;
        }
    }

    return ret;
}

void print_gpx() {
  int i, j;

  if (start) {

      if (option_raw == 2) {
           for (i = 0; i < 9; i++) {
               printf(" %s", dat_str[i]);
           }
           for (i = 0; i < 9; i++) {
               for (j = 0; j < 13; j++) dat_str[i][j] = ' ';
           }
      }
      else {
          if (option_auto && option_verbose) printf("[%c] ", option_inv?'-':'+');
          printf("[%3d] ", gpx.frnr);
          printf("%4d-%02d-%02d ", gpx.jahr, gpx.monat, gpx.tag);
          printf("%02d:%02d:%04.1f ", gpx.std, gpx.min, gpx.sek);
          printf("  ");
          printf("lat: %.6f  ", gpx.lat);
          printf("lon: %.6f  ", gpx.lon);
          printf("alt: %.1f  ", gpx.alt);
          printf(" vH: %5.2f ", gpx.horiV);
          printf(" D: %5.1f ", gpx.dir);
          printf(" vV: %5.2f ", gpx.vertV);
          if (option_ptu  &&  ptu_out) {
              float t = get_Temp(gpx.meas24);
              if (t > -270.0) printf("  T=%.1fC ", t);
              if (option_verbose == 2) {
                  float t2 = get_Temp2(gpx.meas24);
                  float t4 = get_Temp4(gpx.meas24);
                  if (t2 > -270.0) printf("  T2=%.1fC ", t2);
                  if (t4 > -270.0) printf(" T4=%.1fC  ", t4);
                  printf(" f0: %.2f ", gpx.meas24[0]);
                  printf(" f3: %.2f ", gpx.meas24[3]);
                  printf(" f4: %.2f ", gpx.meas24[4]);
              }
          }
          if (option_verbose == 2  &&  (gpx.sonde_typ & 0xF) == 0xA) {
              printf("  U: %.2fV ", gpx.status[0]);
              printf("  Ti: %.1fK ", gpx.status[1]);
          }
            if (option_verbose)
            {
                if (gpx.sonde_typ & SNbit) {
                    printf(" (%s) ", gpx.sonde_id);
                    gpx.sonde_typ ^= SNbit;
                }
            }
      }
      printf("\n");

  }
}

void print_frame() {
    int i;
    int nib = 0;
    int frid = -1;
    int ret0, ret1, ret2;

    if (option_b < 2) {
        manchester1(frame_rawbits, frame_bits, RAWBITFRAME_LEN);
    }


    if (option_ecc == 2) {
        soft_deinterleave(frm+CONF,  7, soft_hamming_conf);
        soft_deinterleave(frm+DAT1, 13, soft_hamming_dat1);
        soft_deinterleave(frm+DAT2, 13, soft_hamming_dat2);

        ret0 = soft_hamming(soft_hamming_conf,  7, block_conf);
        ret1 = soft_hamming(soft_hamming_dat1, 13, block_dat1);
        ret2 = soft_hamming(soft_hamming_dat2, 13, block_dat2);
    }
    else {
        deinterleave(frame_bits+CONF,  7, hamming_conf);
        deinterleave(frame_bits+DAT1, 13, hamming_dat1);
        deinterleave(frame_bits+DAT2, 13, hamming_dat2);

        ret0 = hamming(hamming_conf,  7, block_conf);
        ret1 = hamming(hamming_dat1, 13, block_dat1);
        ret2 = hamming(hamming_dat2, 13, block_dat2);
    }



    if (option_raw == 1) {

        for (i = 0; i < 7; i++) {
            nib = bits2val(block_conf+S*i, S);
            printf("%01X", nib & 0xFF);
        }
        if (option_ecc) {
            if      (ret0 == 0) printf(" [OK] ");
            else if (ret0  > 0) printf(" [KO] ");
            else                printf(" [NO] ");
        }
        printf("  ");
        for (i = 0; i < 13; i++) {
            nib = bits2val(block_dat1+S*i, S);
            printf("%01X", nib & 0xFF);
        }
        if (option_ecc) {
            if      (ret1 == 0) printf(" [OK] ");
            else if (ret1  > 0) printf(" [KO] ");
            else                printf(" [NO] ");
        }
        printf("  ");
        for (i = 0; i < 13; i++) {
            nib = bits2val(block_dat2+S*i, S);
            printf("%01X", nib & 0xFF);
        }
        if (option_ecc) {
            if      (ret2 == 0) printf(" [OK] ");
            else if (ret2  > 0) printf(" [KO] ");
            else                printf(" [NO] ");
        }
        printf("\n");

    }
    else if (option_ecc) {

        if (ret0 == 0 || ret0 > 0 || option_ecc == 2) {
            conf_out(block_conf);
        }
        if (ret1 == 0 || ret1 > 0 || option_ecc == 2) {
            frid = dat_out(block_dat1);
            if (frid == 8) print_gpx();
        }
        if (ret2 == 0 || ret2 > 0 || option_ecc == 2) {
            frid = dat_out(block_dat2);
            if (frid == 8) print_gpx();
        }

    }
    else {

        conf_out(block_conf);
        frid = dat_out(block_dat1);
        if (frid == 8) print_gpx();
        frid = dat_out(block_dat2);
        if (frid == 8) print_gpx();

    }

}

/* -------------------------------------------------------------------------- */


int main(int argc, char **argv) {

    FILE *fp;
    char *fpname;
    int pos, i, j, bit, len;
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
            fprintf(stderr, "       -v, -vv\n");
            fprintf(stderr, "       -r, --raw\n");
            fprintf(stderr, "       -i, --invert\n");
            fprintf(stderr, "       --ecc        (Hamming ECC)\n");
            fprintf(stderr, "       --avg        (moving average)\n");
            fprintf(stderr, "       -b           (alt. Demod.)\n");
            return 0;
        }
        else if ( (strcmp(*argv, "-v") == 0) || (strcmp(*argv, "--verbose") == 0) ) {
            option_verbose = 1;
        }
        else if ( (strcmp(*argv, "-vv") == 0) ) {
            option_verbose = 2;
        }
        else if ( (strcmp(*argv, "-r") == 0) || (strcmp(*argv, "--raw") == 0) ) {
            option_raw = 1;
        }
        else if ( (strcmp(*argv, "-R") == 0) || (strcmp(*argv, "--RAW") == 0) ) {
            option_raw = 2;
        }
        else if ( (strcmp(*argv, "-i") == 0) || (strcmp(*argv, "--invert") == 0) ) {
            option_inv = 0x1;
        }
        else if ( (strcmp(*argv, "--auto") == 0) ) {
            option_auto = 1;
        }
        else if ( (strcmp(*argv, "--avg") == 0) ) {
            option_avg = 1;
        }
        else if   (strcmp(*argv, "-b" ) == 0) { option_b = 1; }
        else if   (strcmp(*argv, "-b2") == 0) { option_b = 2; }
        else if   (strcmp(*argv, "-b3") == 0) { option_b = 3; }
        else if ( (strcmp(*argv, "--ecc" ) == 0) ) { option_ecc = 1; }
        else if ( (strcmp(*argv, "--ecc2") == 0) ) { option_ecc = 2; }
        else if ( (strcmp(*argv, "--ptu") == 0) ) {
            option_ptu = 1;
            //ptu_out = 1; // force ptu (non PS-15)
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

    if (option_ecc == 2) option_b = 2;

    i = read_wav_header(fp);
    if (i) {
        fclose(fp);
        return -1;
    }

    if (option_b > 2) {
        wc = (float*)calloc( 2*(int)(samples_per_bit+1), sizeof(float));
        for (i = 0; i < 2*samples_per_bit; i++) wc[i] =  (i < samples_per_bit) ? 1 : -1; // wie -b2
        //for (i = 0; i < 2*samples_per_bit; i++) wc[i] = sin(2*M_PI*i/(2*samples_per_bit));
        //for (i = 0; i < 2*samples_per_bit; i++) wc[i] = cos(M_PI*i/(2*samples_per_bit));
    }


    if (option_ecc == 2) {
        ui8_t nib, msg[4], code[8];
        for (nib = 0; nib < 16; nib++) {
            nib4bits(nib, msg);
            gencode(msg, code);
            for (i = 0; i < 8; i++)  codewords[nib][i] = code[i];
        }
    }


    for (i = 0; i < 9; i++) {
        for (j = 0; j < 13; j++) dat_str[i][j] = ' ';
    }


    pos = FRAMESTART;

    while (!read_bits_fsk(fp, &bit, &len)) {

        if (len == 0) { // reset_frame();
            if (pos > RAWBITFRAME_LEN-10) { // Problem wegen Interleaving
                print_frame();//byte_count
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
                if (header_found < 0) option_inv ^= 0x1;

            }
            else {
                frame_rawbits[pos] = 0x30 + bit;  // Ascii
                pos++;

                if (pos == RAWBITFRAME_LEN) {
                    frame_rawbits[pos] = '\0';
                    print_frame();//FRAME_LEN
                    header_found = 0;
                    pos = FRAMESTART;
                }
            }

        }
        if (header_found && option_b==1) {
            bitstart = 1;

            while ( pos < RAWBITFRAME_LEN ) {
                if (read_rawbit(fp, &bit) == EOF) break;
                frame_rawbits[pos] = 0x30 + bit;
                pos++;
            }
            frame_rawbits[pos] = '\0';
            print_frame();//FRAME_LEN

            header_found = 0;
            pos = FRAMESTART;
        }
        if (header_found && option_b>=2) {
            bitstart = 1;

            if (pos%2) {
                if (read_rawbit(fp, &bit) == EOF) break;
                    frame_rawbits[pos] = 0x30 + bit;
                    pos++;
            }

            manchester1(frame_rawbits, frame_bits, pos);
            pos /= 2;

            if (option_ecc == 2) {
                for (i = 0; i < pos; i++) {
                    frm[i].hb = frame_bits[i] % 1;
                    frm[i].sb = 0.0; // (ecc2) bit=1: sb>0 , bit=0: sb<0
                }
            }

            while ( pos < BITFRAME_LEN ) {
                if (option_b == 2) {
                    if (option_ecc == 2) {
                        if (soft_read_rawbit2(fp, frm+pos) == EOF) break;
                        bit = frm[pos].hb;
                    }
                    else {
                        if (read_rawbit2(fp, &bit) == EOF) break;
                    }
                }
                else { // option_b==3
                    if (read_rawbit3(fp, &bit) == EOF) break;
                }
                frame_bits[pos] = 0x30 + bit;
                pos++;
            }
            frame_bits[pos] = '\0';
            print_frame();//FRAME_LEN

            header_found = 0;
            pos = FRAMESTART;
        }
    }

    if (option_b > 2) {
        if (wc) free(wc); wc = NULL;
    }

    fclose(fp);

    return 0;
}

