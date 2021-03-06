// Copyright © 1996-2018, Valve Corporation, All rights reserved.

#undef fopen

#include "voice_wavefile.h"

#include <cstdio>

#include "tier0/include/memdbgon.h"

static unsigned long ReadDWord(FILE *fp) {
  unsigned long ret;
  fread(&ret, 4, 1, fp);
  return ret;
}

static u16 ReadWord(FILE *fp) {
  u16 ret;
  fread(&ret, 2, 1, fp);
  return ret;
}

static void WriteDWord(FILE *fp, unsigned long val) { fwrite(&val, 4, 1, fp); }

static void WriteWord(FILE *fp, u16 val) { fwrite(&val, 2, 1, fp); }

bool ReadWaveFile(const ch *pFilename, ch *&pData, int &nDataBytes,
                  int &wBitsPerSample, int &nChannels, int &nSamplesPerSec) {
  FILE *fp;
  if (fopen_s(&fp, pFilename, "rb")) return false;

  fseek(fp, 22, SEEK_SET);

  nChannels = ReadWord(fp);
  nSamplesPerSec = ReadDWord(fp);

  fseek(fp, 34, SEEK_SET);
  wBitsPerSample = ReadWord(fp);

  fseek(fp, 40, SEEK_SET);
  nDataBytes = ReadDWord(fp);
  ReadDWord(fp);
  pData = new ch[nDataBytes];
  if (!pData) {
    fclose(fp);
    return false;
  }
  fread(pData, nDataBytes, 1, fp);
  fclose(fp);
  return true;
}

bool WriteWaveFile(const ch *pFilename, const ch *pData, int nBytes,
                   int wBitsPerSample, int nChannels, int nSamplesPerSec) {
  FILE *fp;
  if (fopen_s(&fp, pFilename, "wb")) return false;

  // Write the RIFF chunk.
  fwrite("RIFF", 4, 1, fp);
  WriteDWord(fp, 0);
  fwrite("WAVE", 4, 1, fp);

  // Write the FORMAT chunk.
  fwrite("fmt ", 4, 1, fp);

  WriteDWord(fp, 0x10);
  WriteWord(fp, 1);  // WAVE_FORMAT_PCM
  WriteWord(fp, (u16)nChannels);
  WriteDWord(fp, (unsigned long)nSamplesPerSec);
  WriteDWord(
      fp, (unsigned long)((wBitsPerSample / 8) * nChannels * nSamplesPerSec));
  WriteWord(fp, (u16)((wBitsPerSample / 8) * nChannels));
  WriteWord(fp, (u16)wBitsPerSample);

  // Write the DATA chunk.
  fwrite("data", 4, 1, fp);
  WriteDWord(fp, (unsigned long)nBytes);
  fwrite(pData, nBytes, 1, fp);

  // Go back and write the length of the riff file.
  unsigned long dwVal = ftell(fp) - 8;
  fseek(fp, 4, SEEK_SET);
  WriteDWord(fp, dwVal);

  fclose(fp);
  return true;
}
