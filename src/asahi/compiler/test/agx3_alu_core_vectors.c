/* GENERATED. Witness-A hand-packer bytes per ALU op/encoding for the no-wait
 * equivalence check (agx3_alu_compose(wait=0) == the compact packer). */
#include <stdint.h>
#include <stddef.h>
struct av{int op,enc; uint8_t n; const uint8_t*b;};
static const uint8_t ab0[]={28,180,2,0,8,8,0,0};
static const uint8_t ab1[]={28,180,2,0,8,8,0,0};
static const uint8_t ab2[]={159,1,86,2,2,8,24,168,19,5};
static const uint8_t ab3[]={63,0,4,58,33,0,163,2,168,129,1,0};
static const uint8_t ab4[]={31,1,86,2,2,8,24,168,17,5};
static const uint8_t ab5[]={63,0,4,58,33,0,162,2,136,129,1,0};
static const uint8_t ab6[]={159,0,86,2,2,8,24,64,208,33,42,0};
static const uint8_t ab7[]={63,0,4,58,33,0,161,2,136,129,64,0,8,0};
static const uint8_t ab8[]={159,0,86,2,2,8,24,0,208,32,10,0};
static const uint8_t ab9[]={63,0,4,58,33,0,161,2,136,129,0,0,1,2};
static const uint8_t ab10[]={25,7,4,5,0,32};
static const uint8_t ab11[]={25,7,4,5,0,32};
static const uint8_t ab12[]={25,7,29,5,0,192};
static const uint8_t ab13[]={25,7,29,5,0,32};
static const uint8_t ab14[]={25,7,30,5,129,8,2,192};
static const uint8_t ab15[]={57,2,7,26,32,32,163,2,56,1,160,0,8,8,0,128};
static const uint8_t ab16[]={18,5,30,7,1,192};
static const uint8_t ab17[]={50,0,71,43,35,0,160,2,150,1,161,96,32,64};
static const uint8_t ab18[]={18,5,30,7,0,192};
static const uint8_t ab19[]={50,0,7,43,35,0,160,2,150,1,161,100,32,64};
static const uint8_t ab20[]={167,17,84,2,2,8,24,24,226,0};
static const uint8_t ab21[]={55,4,0,58,34,0,163,2,136,1,64,0};
static const uint8_t ab22[]={39,16,84,2,2,8,24,0,240,16,5,0};
static const uint8_t ab23[]={55,4,0,58,34,0,160,2,136,129,0,52,0,0};
static const uint8_t ab24[]={167,16,84,2,2,8,24,0,240,16,5,0};
static const uint8_t ab25[]={55,4,0,58,34,0,161,2,136,129,0,52,0,0};
static const uint8_t ab26[]={27,5,15,7,0,0,0,128,0,0};
static const uint8_t ab27[]={59,0,7,128,48,112,160,2,132,129};
static const uint8_t ab28[]={27,5,15,7,2,8,0,128,0,0};
static const uint8_t ab29[]={59,0,7,128,50,120,160,2,132,129};
static const uint8_t ab30[]={27,5,14,7,2,8,0,128,0,0};
static const uint8_t ab31[]={59,0,6,128,50,120,160,2,132,129};
static const uint8_t ab32[]={27,5,38,0,1,8,0,128,0,0};
static const uint8_t ab33[]={59,0,6,58,41,8,168,2};
static const uint8_t ab34[]={175,0,86,2,2,8,16,72,32,0};
static const uint8_t ab35[]={63,0,4,56,34,32,161,2,48,0};
static const uint8_t ab36[]={175,1,86,2,2,8,176,64,0,0};
static const uint8_t ab37[]={63,0,4,56,34,32,163,2,48,0};
static const uint8_t ab38[]={47,0,86,2,3,8,176,64,0,0};
static const uint8_t ab39[]={63,0,4,56,42,32,160,2,48,0};
static const uint8_t ab40[]={167,7,86,2,2,8,172,96};
static const uint8_t ab41[]={63,0,0,18,34,128,175,2,104,0};
static const uint8_t ab42[]={167,7,86,2,2,8,172,32};
static const uint8_t ab43[]={63,0,0,18,34,128,175,2,40,0};
static const uint8_t ab44[]={39,7,86,2,2,8,180,72,3,0};
static const uint8_t ab45[]={63,0,0,58,34,0,174,2,112,1};
static const struct av avs[]={
  {0,0,8,ab0},
  {0,1,8,ab1},
  {1,0,10,ab2},
  {1,1,12,ab3},
  {2,0,10,ab4},
  {2,1,12,ab5},
  {3,0,12,ab6},
  {3,1,14,ab7},
  {4,0,12,ab8},
  {4,1,14,ab9},
  {5,0,6,ab10},
  {5,1,6,ab11},
  {6,0,6,ab12},
  {6,1,6,ab13},
  {7,0,8,ab14},
  {7,1,16,ab15},
  {8,0,6,ab16},
  {8,1,14,ab17},
  {9,0,6,ab18},
  {9,1,14,ab19},
  {10,0,10,ab20},
  {10,1,12,ab21},
  {11,0,12,ab22},
  {11,1,14,ab23},
  {12,0,12,ab24},
  {12,1,14,ab25},
  {13,0,10,ab26},
  {13,1,10,ab27},
  {14,0,10,ab28},
  {14,1,10,ab29},
  {15,0,10,ab30},
  {15,1,10,ab31},
  {16,0,10,ab32},
  {16,1,8,ab33},
  {17,0,10,ab34},
  {17,1,10,ab35},
  {18,0,10,ab36},
  {18,1,10,ab37},
  {19,0,10,ab38},
  {19,1,10,ab39},
  {20,0,8,ab40},
  {20,1,10,ab41},
  {21,0,8,ab42},
  {21,1,10,ab43},
  {22,0,10,ab44},
  {22,1,10,ab45},
};
static const size_t avn=46;
