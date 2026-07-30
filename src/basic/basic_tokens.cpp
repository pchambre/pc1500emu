// Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
// Version 2.0 -- see LICENSE.
#include "basic_tokens.h"

#include <iterator>

namespace pc1500::basic {

// clang-format off
const TokenEntry kTokenTable[] = {
    {0xF170, "ABS"},      {0xF174, "ACS"},      {0xF150, "AND"},
    {0xF180, "AREAD"},    {0xF181, "ARUN"},     {0xF160, "ASC"},
    {0xF173, "ASN"},      {0xF175, "ATN"},      {0xF182, "BEEP"},
    {0xF0B3, "BREAK"},    {0xF18A, "CALL"},     {0xF0B2, "CHAIN"},
    {0xF163, "CHR$"},     {0xF187, "CLEAR"},    {0xF089, "CLOAD"},
    {0xF088, "CLS"},      {0xE858, "COM$"},     {0xF0B1, "CONSOLE"},
    {0xF183, "CONT"},     {0xF0B5, "COLOR"},    {0xF17E, "COS"},
    {0xF095, "CSAVE"},    {0xE680, "CSIZE"},    {0xF084, "CURSOR"},
    {0xF18D, "DATA"},     {0xF165, "DEG"},      {0xF18C, "DEGREE"},
    {0xE857, "DEV$"},     {0xF18B, "DIM"},      {0xF166, "DMS"},
    {0xE884, "DTE"},      {0xF18E, "END"},      {0xF053, "ERL"},
    {0xF052, "ERN"},      {0xF1B4, "ERROR"},    {0xF178, "EXP"},
    {0xF0B0, "FEED"},     {0xF1A5, "FOR"},      {0xF093, "GCURSOR"},
    {0xE682, "GLCURSOR"}, {0xF194, "GOSUB"},    {0xF192, "GOTO"},
    {0xF09F, "GPRINT"},   {0xF186, "GRAD"},     {0xE681, "GRAPH"},
    {0xF196, "IF"},       {0xF15C, "INKEY$"},   {0xF091, "INPUT"},
    {0xE859, "INSTAT"},   {0xF171, "INT"},

    {0xE683, "LCURSOR"},  {0xF17A, "LEFT$"},    {0xF164, "LEN"},
    {0xF198, "LET"},      {0xF0B6, "LF"},       {0xF0B7, "LINE"},
    {0xF090, "LIST"},     {0xF0B8, "LLIST"},    {0xF176, "LN"},
    {0xF1B5, "LOCK"},     {0xF177, "LOG"},      {0xF0B9, "LPRINT"},
    {0xF158, "MEM"},      {0xF08F, "MERGE"},    {0xF17B, "MID$"},
    {0xF19B, "NEW"},      {0xF19A, "NEXT"},     {0xF16D, "NOT"},
    {0xF19E, "OFF"},      {0xF19C, "ON"},       {0xF19D, "OPN"},
    {0xF151, "OR"},       {0xE880, "OUTSTAT"},  {0xF1A2, "PAUSE"},
    {0xF16F, "PEEK"},     {0xF16E, "PEEK#"},    {0xF15D, "PI"},
    {0xF168, "POINT"},    {0xF1A1, "POKE"},     {0xF1A0, "POKE#"},
    {0xF097, "PRINT"},    {0xF1AA, "RADIAN"},   {0xF1A8, "RANDOM"},
    {0xF1A6, "READ"},     {0xF1AB, "REM"},      {0xF1A7, "RESTORE"},
    {0xF199, "RETURN"},   {0xF172, "RIGHT$"},   {0xE85A, "RINKEY$"},
    {0xF0BA, "RLINE"},    {0xE7A9, "RMT"},      {0xF17C, "RND"},
    {0xE685, "ROTATE"},   {0xF1A4, "RUN"},

    {0xE882, "SETCOM"},   {0xE886, "SETDEV"},   {0xF179, "SGN"},
    {0xF17D, "SIN"},      {0xE684, "SORGN"},    {0xF061, "SPACE$"},
    {0xF16B, "SQR"},      {0xF167, "STATUS"},   {0xF1AD, "STEP"},
    {0xF1AC, "STOP"},     {0xF161, "STR$"},     {0xF0BB, "TAB"},
    {0xF17F, "TAN"},      {0xE883, "TERMINAL"}, {0xF0BC, "TEST"},
    {0xE686, "TEXT"},     {0xF1AE, "THEN"},     {0xF15B, "TIME"},
    {0xF1B1, "TO"},       {0xE885, "TRANSMIT"}, {0xF1B0, "TROFF"},
    {0xF1AF, "TRON"},     {0xF1B6, "UNLOCK"},   {0xF085, "USING"},
    {0xF162, "VAL"},      {0xF1B3, "WAIT"},     {0xF0B4, "ZONE"},
};
// clang-format on

const size_t kTokenTableSize = std::size(kTokenTable);

const char* keywordForCode(uint16_t code) {
  for (size_t i = 0; i < kTokenTableSize; i++) {
    if (kTokenTable[i].code == code) return kTokenTable[i].keyword;
  }
  return nullptr;
}

}  // namespace pc1500::basic
