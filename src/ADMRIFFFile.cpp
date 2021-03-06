
#include <math.h>

#include <string>

#define DEBUG_LEVEL 1
#include <bbcat-base/PerformanceMonitor.h>

#include "ADMRIFFFile.h"
#include "RIFFChunk_Definitions.h"

BBC_AUDIOTOOLBOX_START

ADMRIFFFile::ADMRIFFFile() : RIFFFile(),
                             adm(NULL)
{
}

ADMRIFFFile::~ADMRIFFFile()
{
  Close();
}

/*--------------------------------------------------------------------------------*/
/** Open a WAVE/RIFF file
 *
 * @param filename filename of file to open
 *
 * @return true if file opened and interpreted correctly (including any extra chunks if present)
 */
/*--------------------------------------------------------------------------------*/
bool ADMRIFFFile::Open(const char *filename)
{
  bool success = false;

  if ((adm = ADMData::Create()) != NULL)
  {
    success = RIFFFile::Open(filename);
  }
  else ERROR("No providers for ADM XML decoding!");

  return success;
}

/*--------------------------------------------------------------------------------*/
/** Create empty ADM and populate basic track information
 *
 * @return true if successful
 */
/*--------------------------------------------------------------------------------*/
bool ADMRIFFFile::CreateADM()
{
  bool success = false;

  if (IsOpen() && !adm)
  {
    if ((adm = ADMData::Create()) != NULL)
    {
      uint_t i, nchannels = GetChannels();

      for (i = 0; i < nchannels; i++)
      {
        ADMAudioTrack *track;
        std::string name;

        Printf(name, "Track %u", i + 1);

        // create chna track data
        if ((track = adm->CreateTrack(name)) != NULL)
        {
          track->SetTrackNum(i);
          track->SetSampleRate(GetSampleRate());
          track->SetBitDepth(GetBitsPerSample());
        }
      }

      success = true;
    }
    else ERROR("No providers for ADM XML decoding!");
  }

  return success;
}

/*--------------------------------------------------------------------------------*/
/** Close RIFF file, writing chunks if file was opened for writing 
 *
 * @param abortwrite true to abort the writing of file
 *
 * @note this may take some time because it copies sample data from a temporary file
 */
/*--------------------------------------------------------------------------------*/
void ADMRIFFFile::Close(bool abortwrite)
{
  uint_t i;

  if (file && adm && writing && !abortwrite)
  {
    RIFFChunk *chunk;
    uint64_t  endtime = filesamples ? filesamples->GetAbsolutePositionNS() : 0;
    uint32_t  chnalen;
    uint8_t   *chna;

    DEBUG1(("Finalising ADM for '%s'...", file->getfilename().c_str()));

    adm->SortTracks();
    adm->ConnectReferences();
    adm->ChangeTemporaryIDs();

    DEBUG1(("Finishing all blockformats"));

    // complete BlockFormats on all channels
    for (i = 0; i < cursors.size(); i++)
    {
      cursors[i]->Seek(endtime);
      cursors[i]->EndChanges();
    }

    DEBUG1(("Creating ADM RIFF chunks"));

    // get ADM object to create chna chunk
    if ((chna = adm->GetChna(chnalen)) != NULL)
    {
      // and add it to the RIFF file
      if ((chunk = AddChunk(chna_ID)) != NULL)
      {
        chunk->CreateChunkData(chna, chnalen);
      }
      else ERROR("Failed to add chna chunk");

      // don't need the raw data any more
      delete[] chna;
    }
    else ERROR("No chna data available");

    // add axml chunk
    if ((chunk = AddChunk(axml_ID)) != NULL)
    {
      // create axml data
      std::string str = adm->GetAxml();

      // set chunk data
      chunk->CreateChunkData(str.c_str(), str.size());

      DEBUG3(("AXML: %s", str.c_str()));
    }
    else ERROR("Failed to add axml chunk");
  }

  // write chunks, copy samples and close file
  RIFFFile::Close(abortwrite);

  for (i = 0; i < cursors.size(); i++)
  {
    delete cursors[i];
  }
  cursors.clear();

  if (adm)
  {
    adm->Delete();
    delete adm;
    adm = NULL;
  }
}

/*--------------------------------------------------------------------------------*/
/** Create ADM from text file
 *
 * @param filename text filename (see below for format)
 *
 * @return true if successful
 *
 * The file MUST be of the following format with each entry on its own line:
 * <ADM programme name>[:<ADM content name>]
 *
 * then for each track:
 * <track>:<trackname>:<objectname>
 *
 * Where <track> is 1..number of tracks available within ADM
 */
/*--------------------------------------------------------------------------------*/
bool ADMRIFFFile::CreateADM(const char *filename)
{
  bool success = false;

  if (CreateADM())
  {
    // create ADM structure (content and objects from file)
    if (adm->CreateFromFile(filename))
    {
      // can prepare cursors now since all objects have been created
      PrepareCursors();

      success = true;
    }
    else ERROR("Unable to create ADM structure from '%s'", filename);
  }

  return success;
}

/*--------------------------------------------------------------------------------*/
/** Create cursors and add all objects to each cursor
 *
 * @note this can be called prior to writing samples or setting positions but it
 * @note *will* be called by SetPositions() if not done so already
 */
/*--------------------------------------------------------------------------------*/
void ADMRIFFFile::PrepareCursors()
{
  if (cursors.size() == 0)
  {
    std::vector<const ADMAudioObject *> objects;
    ADMTrackCursor *cursor;
    uint_t i, nchannels = GetChannels();

    // get list of ADMAudioObjects
    adm->GetAudioObjectList(objects);

    // add all objects to all cursors
    for (i = 0; i < nchannels; i++)
    {
      // create track cursor for tracking position during writing
      if ((cursor = new ADMTrackCursor(i)) != NULL) {
        cursor->Add(objects);
        cursors.push_back(cursor);
      }
    }
  }
}

bool ADMRIFFFile::PostReadChunks()
{
  bool success = RIFFFile::PostReadChunks();

  // after reading of chunks, find chna and axml chunks and decode them
  // to create an ADM
  if (success)
  {
    RIFFChunk *chna = GetChunk(chna_ID);
    RIFFChunk *axml = GetChunk(axml_ID);

    // ensure each chunk is valid
    if (adm &&
        chna && chna->GetData() &&
        axml && axml->GetData())
    {
      // decode chunks
      success = adm->Set(chna->GetData(), chna->GetLength(), axml->GetData(), axml->GetLength());

#if DEBUG_LEVEL >= 4
      { // dump ADM as text
        std::string str;
        adm->Dump(str);
                
        DEBUG("%s", str.c_str());
      }

      { // dump ADM as XML
        std::string str;
        adm->GenerateXML(str);
                
        DEBUG("%s", str.c_str());
      }

      DEBUG("Audio objects:");
      std::vector<const ADMObject *> list;
      adm->GetADMList(ADMAudioObject::Type, list);
      uint_t i;
      for (i = 0; i < list.size(); i++)
      {
        DEBUG("%s", list[i]->ToString().c_str());
      }
#endif
    }
    // test for different types of failure
    else if (!adm)
    {
      ERROR("Cannot decode ADM, no ADM decoder available");
      success = false;
    }
    else if (!chna && !axml)
    {
      // acceptable failure - neither chna nor axml chunk specified - not an ADM compatible BWF file but open anyway
      DEBUG("Warning: no chna or axml chunks!");
      success = true;
    }
    else {
      // unacceptible failures: empty chna or empty axml chunks
      if (chna && !chna->GetData()) ERROR("Cannot decode ADM, chna chunk not available");
      if (axml && !axml->GetData()) ERROR("Cannot decode ADM, axml chunk not available");
      success = false;
    }

    // now that the data is dealt with, the chunk data can be deleted
    if (axml) axml->DeleteData();
    if (chna) chna->DeleteData();
  }

  return success;
}

void ADMRIFFFile::UpdateSamplePosition()
{
}

/*--------------------------------------------------------------------------------*/
/** Set parameters of channel during writing
 *
 * @param channel channel to change the position of
 * @param objparameters object parameters
 */
/*--------------------------------------------------------------------------------*/
void ADMRIFFFile::SetObjectParameters(uint_t channel, const AudioObjectParameters& objparameters)
{
  if (cursors.size() == 0)
  {
    // create cursors and add all objects to them
    PrepareCursors();
  }

  if (writing && (channel < cursors.size()))
  {
    PERFMON("Write ADM Channel Parameters");
    uint64_t t = filesamples ? filesamples->GetAbsolutePositionNS() : 0;

    cursors[channel]->Seek(t);
    cursors[channel]->SetObjectParameters(objparameters);
  }
}

BBC_AUDIOTOOLBOX_END
