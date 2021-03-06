/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#if (defined HAVE_CONFIG_H) && (!defined TARGET_WINDOWS)
  #include "config.h"
#elif defined(TARGET_WINDOWS)
#include "system.h"
#endif

#include "OMXImage.h"

#include "utils/log.h"
#include "linux/XMemUtils.h"

#include "utils/BitstreamConverter.h"

#include <sys/time.h>
#include <inttypes.h>
#include "guilib/GraphicContext.h"
#include "settings/AdvancedSettings.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"

#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "COMXImage"

#define CONTENTURI_MAXLEN 256

#define EXIF_TAG_ORIENTATION    0x0112

static CCriticalSection g_OMXSection;

COMXImage::COMXImage()
{
  m_is_open       = false;
  m_image_size    = 0;
  m_image_buffer  = NULL;
  m_progressive   = false;
  m_alpha         = false;
  m_orientation   = 0;
  m_width         = 0;
  m_height        = 0;

  m_decoded_buffer = NULL;
  m_encoded_buffer = NULL;

  m_decoder_open  = false;
  m_encoder_open  = false;

  OMX_INIT_STRUCTURE(m_decoded_format);
  OMX_INIT_STRUCTURE(m_encoded_format);
  memset(&m_omx_image, 0x0, sizeof(OMX_IMAGE_PORTDEFINITIONTYPE));
}

COMXImage::~COMXImage()
{
  Close();
}

void COMXImage::Close()
{
  CSingleLock lock(g_OMXSection);

  OMX_INIT_STRUCTURE(m_decoded_format);
  OMX_INIT_STRUCTURE(m_encoded_format);
  memset(&m_omx_image, 0x0, sizeof(OMX_IMAGE_PORTDEFINITIONTYPE));

  if(m_image_buffer)
    free(m_image_buffer);

  m_image_buffer  = NULL;
  m_image_size    = 0;
  m_width         = 0;
  m_height        = 0;
  m_is_open       = false;
  m_progressive   = false;
  m_orientation   = 0;
  m_decoded_buffer = NULL;
  m_encoded_buffer = NULL;

  if(m_decoder_open)
  {
    m_omx_decoder.FlushInput();
    m_omx_decoder.FreeInputBuffers();
    m_omx_resize.FlushOutput();
    m_omx_resize.FreeOutputBuffers();

    m_omx_tunnel_decode.Flush();
    m_omx_tunnel_decode.Flush();
    m_omx_tunnel_decode.Deestablish();
    m_omx_decoder.Deinitialize();
    m_omx_resize.Deinitialize();
    m_decoder_open = false;
  }

  if(m_encoder_open)
  {
    m_omx_encoder.Deinitialize();
    m_encoder_open = false;
  }

  m_pFile.Close();
}

typedef enum {      /* JPEG marker codes */
  M_SOF0  = 0xc0,
  M_SOF1  = 0xc1,
  M_SOF2  = 0xc2,
  M_SOF3  = 0xc3,
  M_SOF5  = 0xc5,
  M_SOF6  = 0xc6,
  M_SOF7  = 0xc7,
  M_JPG   = 0xc8,
  M_SOF9  = 0xc9,
  M_SOF10 = 0xca,
  M_SOF11 = 0xcb,
  M_SOF13 = 0xcd,
  M_SOF14 = 0xce,
  M_SOF15 = 0xcf,

  M_DHT   = 0xc4,
  M_DAC   = 0xcc,

  M_RST0  = 0xd0,
  M_RST1  = 0xd1,
  M_RST2  = 0xd2,
  M_RST3  = 0xd3,
  M_RST4  = 0xd4,
  M_RST5  = 0xd5,
  M_RST6  = 0xd6,
  M_RST7  = 0xd7,

  M_SOI   = 0xd8,
  M_EOI   = 0xd9,
  M_SOS   = 0xda,
  M_DQT   = 0xdb,
  M_DNL   = 0xdc,
  M_DRI   = 0xdd,
  M_DHP   = 0xde,
  M_EXP   = 0xdf,

  M_APP0  = 0xe0,
  M_APP1  = 0xe1,
  M_APP2  = 0xe2,
  M_APP3  = 0xe3,
  M_APP4  = 0xe4,
  M_APP5  = 0xe5,
  M_APP6  = 0xe6,
  M_APP7  = 0xe7,
  M_APP8  = 0xe8,
  M_APP9  = 0xe9,
  M_APP10 = 0xea,
  M_APP11 = 0xeb,
  M_APP12 = 0xec,
  M_APP13 = 0xed,
  M_APP14 = 0xee,
  M_APP15 = 0xef,
  // extensions
  M_JPG0  = 0xf0,
  M_JPG1  = 0xf1,
  M_JPG2  = 0xf2,
  M_JPG3  = 0xf3,
  M_JPG4  = 0xf4,
  M_JPG5  = 0xf5,
  M_JPG6  = 0xf6,
  M_JPG7  = 0xf7,
  M_JPG8  = 0xf8,
  M_JPG9  = 0xf9,
  M_JPG10 = 0xfa,
  M_JPG11 = 0xfb,
  M_JPG12 = 0xfc,
  M_JPG13 = 0xfd,
  M_JPG14 = 0xfe,
  M_COM   = 0xff,

  M_TEM   = 0x01,
} JPEG_MARKER;

OMX_IMAGE_CODINGTYPE COMXImage::GetCodingType()
{
  memset(&m_omx_image, 0x0, sizeof(OMX_IMAGE_PORTDEFINITIONTYPE));
  m_width         = 0;
  m_height        = 0;
  m_progressive   = false;
  m_orientation   = 0;

  m_omx_image.eCompressionFormat = OMX_IMAGE_CodingMax;

  if(!m_image_size)
    return OMX_IMAGE_CodingMax;

  bits_reader_t br;
  CBitstreamConverter::bits_reader_set( &br, m_image_buffer, m_image_size );

  /* JPEG Header */
  if(CBitstreamConverter::read_bits(&br, 16) == 0xFFD8)
  {
    m_omx_image.eCompressionFormat = OMX_IMAGE_CodingJPEG;

    CBitstreamConverter::read_bits(&br, 8);
    unsigned char marker = CBitstreamConverter::read_bits(&br, 8);
    unsigned short block_size = 0;
    bool nMarker = false;

    while(!br.oflow) {

      switch(marker)
      {
        case M_TEM:
        case M_DRI:
          CBitstreamConverter::skip_bits(&br, 16);
          continue;
        case M_SOI:
        case M_EOI:
          continue;
        
        case M_SOS:
        case M_DQT:
        case M_DNL:
        case M_DHP:
        case M_EXP:

        case M_DHT:

        case M_SOF0:
        case M_SOF1:
        case M_SOF2:
        case M_SOF3:

        case M_SOF5:
        case M_SOF6:
        case M_SOF7:

        case M_JPG:
        case M_SOF9:
        case M_SOF10:
        case M_SOF11:

        case M_SOF13:
        case M_SOF14:
        case M_SOF15:

        case M_APP0:
        case M_APP1:
        case M_APP2:
        case M_APP3:
        case M_APP4:
        case M_APP5:
        case M_APP6:
        case M_APP7:
        case M_APP8:
        case M_APP9:
        case M_APP10:
        case M_APP11:
        case M_APP12:
        case M_APP13:
        case M_APP14:
        case M_APP15:

        case M_JPG0:
        case M_JPG1:
        case M_JPG2:
        case M_JPG3:
        case M_JPG4:
        case M_JPG5:
        case M_JPG6:
        case M_JPG7:
        case M_JPG8:
        case M_JPG9:
        case M_JPG10:
        case M_JPG11:
        case M_JPG12:
        case M_JPG13:
        case M_JPG14:
        case M_COM:
          block_size = CBitstreamConverter::read_bits(&br, 16);
          nMarker = true;
          break;

        default:
          nMarker = false;
          break;
      }

      if(!nMarker)
      {
        break;
      }

      if(marker >= M_SOF0 && marker <= M_SOF15 && marker != M_DHT && marker != M_DAC)
      {
        if(marker == M_SOF2 || marker == M_SOF6 || marker == M_SOF10 || marker == M_SOF14)
        {
          m_progressive = true;
        }
        CBitstreamConverter::skip_bits(&br, 8);
        m_omx_image.nFrameHeight = CBitstreamConverter::read_bits(&br, 16);
        m_omx_image.nFrameWidth = CBitstreamConverter::read_bits(&br, 16);
        CBitstreamConverter::skip_bits(&br, 8 * (block_size - 9));
      }
      else if(marker == M_APP1)
      {
        int readBits = 2;

        // Exif header
        if(CBitstreamConverter::read_bits(&br, 32) == 0x45786966)
        {
          bool bMotorolla = false;
          bool bError = false;
          CBitstreamConverter::skip_bits(&br, 8 * 2);
          readBits += 2;
        
          char o1 = CBitstreamConverter::read_bits(&br, 8);
          char o2 = CBitstreamConverter::read_bits(&br, 8);
          readBits += 2;

          /* Discover byte order */
          if(o1 == 'M' && o2 == 'M')
            bMotorolla = true;
          else if(o1 == 'I' && o2 == 'I')
            bMotorolla = false;
          else
            bError = true;
        
          CBitstreamConverter::skip_bits(&br, 8 * 2);
          readBits += 2;

          if(!bError)
          {
            unsigned int offset, a, b, numberOfTags, tagNumber;
  
            // Get first IFD offset (offset to IFD0)
            if(bMotorolla)
            {
              CBitstreamConverter::skip_bits(&br, 8 * 2);
              readBits += 2;

              a = CBitstreamConverter::read_bits(&br, 8);
              b = CBitstreamConverter::read_bits(&br, 8);
              readBits += 2;
              offset = (a << 8) + b;
            }
            else
            {
              a = CBitstreamConverter::read_bits(&br, 8);
              b = CBitstreamConverter::read_bits(&br, 8);
              readBits += 2;
              offset = (b << 8) + a;

              CBitstreamConverter::skip_bits(&br, 8 * 2);
              readBits += 2;
            }

            offset -= 8;
            if(offset > 0)
            {
              CBitstreamConverter::skip_bits(&br, 8 * offset);
              readBits += offset;
            } 

            // Get the number of directory entries contained in this IFD
            if(bMotorolla)
            {
              a = CBitstreamConverter::read_bits(&br, 8);
              b = CBitstreamConverter::read_bits(&br, 8);
              numberOfTags = (a << 8) + b;
            }
            else
            {
              a = CBitstreamConverter::read_bits(&br, 8);
              b = CBitstreamConverter::read_bits(&br, 8);
              numberOfTags = (b << 8) + a;
            }
            readBits += 2;

            while(numberOfTags && !br.oflow)
            {
              // Get Tag number
              if(bMotorolla)
              {
                a = CBitstreamConverter::read_bits(&br, 8);
                b = CBitstreamConverter::read_bits(&br, 8);
                tagNumber = (a << 8) + b;
                readBits += 2;
              }
              else
              {
                a = CBitstreamConverter::read_bits(&br, 8);
                b = CBitstreamConverter::read_bits(&br, 8);
                tagNumber = (b << 8) + a;
                readBits += 2;
              }

              //found orientation tag
              if(tagNumber == EXIF_TAG_ORIENTATION)
              {
                if(bMotorolla)
                {
                  CBitstreamConverter::skip_bits(&br, 8 * 7);
                  readBits += 7;
                  m_orientation = CBitstreamConverter::read_bits(&br, 8);
                  readBits += 1;
                  CBitstreamConverter::skip_bits(&br, 8 * 2);
                  readBits += 2;
                }
                else
                {
                  CBitstreamConverter::skip_bits(&br, 8 * 6);
                  readBits += 6;
                  m_orientation = CBitstreamConverter::read_bits(&br, 8);
                  readBits += 1;
                  CBitstreamConverter::skip_bits(&br, 8 * 3);
                  readBits += 3;
                }
                break;
              }
              else
              {
                CBitstreamConverter::skip_bits(&br, 8 * 10);
                readBits += 10;
              }
              numberOfTags--;
            }
          }
        }
        readBits += 4;
        CBitstreamConverter::skip_bits(&br, 8 * (block_size - readBits));
      }
      else
      {
        CBitstreamConverter::skip_bits(&br, 8 * (block_size - 2));
      }

      CBitstreamConverter::read_bits(&br, 8);
      marker = CBitstreamConverter::read_bits(&br, 8);

    }

  }

  CBitstreamConverter::bits_reader_set( &br, m_image_buffer, m_image_size );

  /* PNG Header */
  if(CBitstreamConverter::read_bits(&br, 32) == 0x89504E47)
  {
    m_omx_image.eCompressionFormat = OMX_IMAGE_CodingPNG;
    CBitstreamConverter::skip_bits(&br, 32 * 2);
    if(CBitstreamConverter::read_bits(&br, 32) == 0x49484452)
    {
      m_omx_image.nFrameWidth = CBitstreamConverter::read_bits(&br, 32);
      m_omx_image.nFrameHeight = CBitstreamConverter::read_bits(&br, 32);
      (void)CBitstreamConverter::read_bits(&br, 8); // bit depth
      unsigned int coding_type = CBitstreamConverter::read_bits(&br, 8);
      m_alpha = coding_type==4 || coding_type==6;
    }
  }

  if(m_orientation > 8)
    m_orientation = 0;

  m_width  = m_omx_image.nFrameWidth;
  m_height = m_omx_image.nFrameHeight;

  return m_omx_image.eCompressionFormat;
}

bool COMXImage::ClampLimits(unsigned int &width, unsigned int &height)
{
  RESOLUTION_INFO& res_info =  CDisplaySettings::Get().GetResolutionInfo(g_graphicsContext.GetVideoResolution());
  const bool transposed = m_orientation & 4;
  unsigned int max_width = width;
  unsigned int max_height = height;
  const unsigned int gui_width  = transposed ? res_info.iHeight:res_info.iWidth;
  const unsigned int gui_height = transposed ? res_info.iWidth:res_info.iHeight;
  const float aspect = (float)m_width / m_height;

  if (max_width == 0 || max_height == 0)
  {
    max_height = g_advancedSettings.m_imageRes;

    if (g_advancedSettings.m_fanartRes > g_advancedSettings.m_imageRes)
    { // 16x9 images larger than the fanart res use that rather than the image res
      if (fabsf(aspect / (16.0f/9.0f) - 1.0f) <= 0.01f && m_height >= g_advancedSettings.m_fanartRes)
      {
        max_height = g_advancedSettings.m_fanartRes;
      }
    }
    max_width = max_height * 16/9;
  }

  if (gui_width)
    max_width = min(max_width, gui_width);
  if (gui_height)
    max_height = min(max_height, gui_height);

  max_width  = min(max_width, 2048U);
  max_height = min(max_height, 2048U);


  width = m_width;
  height = m_height;
  if (width > max_width || height > max_height)
  {
    if ((unsigned int)(max_width / aspect + 0.5f) > max_height)
      max_width = (unsigned int)(max_height * aspect + 0.5f);
    else
      max_height = (unsigned int)(max_width / aspect + 0.5f);
    width = max_width;
    height = max_height;
    return true;
  }
  return false;
}

bool COMXImage::ReadFile(const CStdString& inputFile)
{
  if(!m_pFile.Open(inputFile, 0))
  {
    CLog::Log(LOGERROR, "%s::%s %s not found\n", CLASSNAME, __func__, inputFile.c_str());
    return false;
  }

  if(m_image_buffer)
    free(m_image_buffer);
  m_image_buffer = NULL;

  m_image_size = m_pFile.GetLength();

  if(!m_image_size) {
    CLog::Log(LOGERROR, "%s::%s %s m_image_size zero\n", CLASSNAME, __func__, inputFile.c_str());
    return false;
  }
  m_image_buffer = (uint8_t *)malloc(m_image_size);
  if(!m_image_buffer) {
    CLog::Log(LOGERROR, "%s::%s %s m_image_buffer null (%lu)\n", CLASSNAME, __func__, inputFile.c_str(), m_image_size);
    return false;
  }
  
  m_pFile.Read(m_image_buffer, m_image_size);

  if(GetCodingType() != OMX_IMAGE_CodingJPEG) {
    CLog::Log(LOGERROR, "%s::%s %s GetCodingType=0x%x\n", CLASSNAME, __func__, inputFile.c_str(), GetCodingType());
    return false;
  }

  if(m_width < 1 || m_height < 1) {
    CLog::Log(LOGERROR, "%s::%s %s m_width=%d m_height=%d\n", CLASSNAME, __func__, inputFile.c_str(), m_width, m_height);
    return false;
  }

  m_is_open = true;

  return true;
}

bool COMXImage::HandlePortSettingChange(unsigned int resize_width, unsigned int resize_height)
{
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  // on the first port settings changed event, we create the tunnel and alloc the buffer
  if (!m_decoded_buffer)
  {
    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    OMX_INIT_STRUCTURE(port_def);

    port_def.nPortIndex = m_omx_decoder.GetOutputPort();
    m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
    port_def.nPortIndex = m_omx_resize.GetInputPort();
    m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);

    m_omx_tunnel_decode.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_resize, m_omx_resize.GetInputPort());

    omx_err = m_omx_tunnel_decode.Establish(false);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_tunnel_decode.Establish\n", CLASSNAME, __func__);
      return false;
    }
    omx_err = m_omx_resize.WaitForEvent(OMX_EventPortSettingsChanged);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.WaitForEvent=%x\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    port_def.nPortIndex = m_omx_resize.GetOutputPort();
    m_omx_resize.GetParameter(OMX_IndexParamPortDefinition, &port_def);

    port_def.nPortIndex = m_omx_resize.GetOutputPort();
    port_def.format.image.eCompressionFormat = OMX_IMAGE_CodingUnused;
    port_def.format.image.eColorFormat = OMX_COLOR_Format32bitARGB8888;
    port_def.format.image.nFrameWidth = resize_width;
    port_def.format.image.nFrameHeight = resize_height;
    port_def.format.image.nStride = resize_width*4;
    port_def.format.image.nSliceHeight = 0;
    port_def.format.image.bFlagErrorConcealment = OMX_FALSE;

    omx_err = m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    OMX_INIT_STRUCTURE(m_decoded_format);
    m_decoded_format.nPortIndex = m_omx_resize.GetOutputPort();
    omx_err = m_omx_resize.GetParameter(OMX_IndexParamPortDefinition, &m_decoded_format);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.GetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }
    assert(m_decoded_format.nBufferCountActual == 1);

    omx_err = m_omx_resize.AllocOutputBuffers();//false, true);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.AllocOutputBuffers result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }
    omx_err = m_omx_resize.SetStateForComponent(OMX_StateExecuting);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.SetStateForComponent result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    m_decoded_buffer = m_omx_resize.GetOutputBuffer();

    if(!m_decoded_buffer)
    {
      CLog::Log(LOGERROR, "%s::%s no output buffer\n", CLASSNAME, __func__);
      return false;
    }

    omx_err = m_omx_resize.FillThisBuffer(m_decoded_buffer);
    if(omx_err != OMX_ErrorNone)
     {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize FillThisBuffer result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }
  }
  // on subsequent port settings changed event, we just copy the port settings
  else
  {
    // a little surprising, make a note
    CLog::Log(LOGDEBUG, "%s::%s m_omx_resize second port changed event\n", CLASSNAME, __func__);
    m_omx_decoder.DisablePort(m_omx_decoder.GetOutputPort(), true);
    m_omx_resize.DisablePort(m_omx_resize.GetInputPort(), true);

    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    OMX_INIT_STRUCTURE(port_def);

    port_def.nPortIndex = m_omx_decoder.GetOutputPort();
    m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
    port_def.nPortIndex = m_omx_resize.GetInputPort();
    m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);

    omx_err = m_omx_resize.WaitForEvent(OMX_EventPortSettingsChanged);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.WaitForEvent=%x\n", CLASSNAME, __func__, omx_err);
      return false;
    }
    m_omx_decoder.EnablePort(m_omx_decoder.GetOutputPort(), true);
    m_omx_resize.EnablePort(m_omx_resize.GetInputPort(), true);
  }
  return true;
}

bool COMXImage::Decode(unsigned width, unsigned height)
{
  CSingleLock lock(g_OMXSection);
  std::string componentName = "";
  unsigned int demuxer_bytes = 0;
  const uint8_t *demuxer_content = NULL;
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  OMX_BUFFERHEADERTYPE *omx_buffer = NULL;

  if(!m_image_buffer)
  {
    CLog::Log(LOGERROR, "%s::%s no input buffer\n", CLASSNAME, __func__);
    return false;
  }

  if(GetCompressionFormat() == OMX_IMAGE_CodingMax)
  {
    CLog::Log(LOGERROR, "%s::%s error unsupported image format\n", CLASSNAME, __func__);
    return false;
  }

  if(IsProgressive())
  {
    CLog::Log(LOGWARNING, "%s::%s progressive images not supported by decoder\n", CLASSNAME, __func__);
    return false;
  }

  if(!m_is_open)
  {
    CLog::Log(LOGERROR, "%s::%s error not opened\n", CLASSNAME, __func__);
    return false;
  }

  componentName = "OMX.broadcom.image_decode";
  if(!m_omx_decoder.Initialize((const std::string)componentName, OMX_IndexParamImageInit))
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_decoder.Initialize\n", CLASSNAME, __func__);
    return false;
  }

  componentName = "OMX.broadcom.resize";
  if(!m_omx_resize.Initialize((const std::string)componentName, OMX_IndexParamImageInit))
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_resize.Initialize\n", CLASSNAME, __func__);
    return false;
  }

  m_decoder_open = true;
  ClampLimits(width, height);

  // set input format
  OMX_IMAGE_PARAM_PORTFORMATTYPE imagePortFormat;
  OMX_INIT_STRUCTURE(imagePortFormat);
  imagePortFormat.nPortIndex = m_omx_decoder.GetInputPort();
  imagePortFormat.eCompressionFormat = OMX_IMAGE_CodingJPEG;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamImagePortFormat, &imagePortFormat);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_decoder.SetParameter OMX_IndexParamImagePortFormat result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_decoder.AllocInputBuffers();
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_decoder.AllocInputBuffers result(0x%x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_decoder.SetStateForComponent result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  demuxer_bytes   = GetImageSize();
  demuxer_content = GetImageBuffer();
  if(!demuxer_bytes || !demuxer_content)
    return false;

  while(demuxer_bytes > 0 || !m_decoded_buffer)
  {
    long timeout = 0;
    if (demuxer_bytes)
    {
       omx_buffer = m_omx_decoder.GetInputBuffer(1000);
       if(omx_buffer == NULL)
         return false;

       omx_buffer->nOffset = omx_buffer->nFlags  = 0;

       omx_buffer->nFilledLen = (demuxer_bytes > omx_buffer->nAllocLen) ? omx_buffer->nAllocLen : demuxer_bytes;
       memcpy(omx_buffer->pBuffer, demuxer_content, omx_buffer->nFilledLen);

       demuxer_content += omx_buffer->nFilledLen;
       demuxer_bytes -= omx_buffer->nFilledLen;

       if(demuxer_bytes == 0)
         omx_buffer->nFlags |= OMX_BUFFERFLAG_EOS;

       omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
       if (omx_err != OMX_ErrorNone)
       {
         CLog::Log(LOGERROR, "%s::%s OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
         return false;
       }
    }
    else
    {
       // we've submitted all buffers so can wait now
       timeout = 1000;
    }
    omx_err = m_omx_decoder.WaitForEvent(OMX_EventPortSettingsChanged, timeout);
    if(omx_err == OMX_ErrorNone)
    {
      if (!HandlePortSettingChange(width, height))
      {
        CLog::Log(LOGERROR, "%s::%s HandlePortSettingChange() failed\n", CLASSNAME, __func__);
        return false;
      }
    }
    // we treat it as an error if a real timeout occurred
    else  if (timeout)
    {
      CLog::Log(LOGERROR, "%s::%s HandlePortSettingChange() failed\n", CLASSNAME, __func__);
      return false;
    }
  }

  omx_err = m_omx_decoder.WaitForEvent(OMX_EventBufferFlag, 1000);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_decoder.WaitForEvent result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  m_omx_tunnel_decode.Deestablish();

  if(m_omx_decoder.BadState())
    return false;

  return true;
}


bool COMXImage::Encode(unsigned char *buffer, int size, unsigned width, unsigned height, unsigned int pitch)
{
  CSingleLock lock(g_OMXSection);

  std::string componentName = "";
  unsigned int demuxer_bytes = 0;
  const uint8_t *demuxer_content = NULL;
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  OMX_BUFFERHEADERTYPE *omx_buffer = NULL;
  OMX_INIT_STRUCTURE(m_encoded_format);

  if (pitch == 0)
     pitch = 4 * width;

  if (!buffer || !size) 
  {
    CLog::Log(LOGERROR, "%s::%s error no buffer\n", CLASSNAME, __func__);
    return false;
  }

  componentName = "OMX.broadcom.image_encode";
  if(!m_omx_encoder.Initialize((const std::string)componentName, OMX_IndexParamImageInit))
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_encoder.Initialize\n", CLASSNAME, __func__);
    return false;
  }

  m_encoder_open = true;

  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  OMX_INIT_STRUCTURE(port_def);
  port_def.nPortIndex = m_omx_encoder.GetInputPort();

  omx_err = m_omx_encoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.GetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  port_def.format.image.eCompressionFormat = OMX_IMAGE_CodingUnused;
  port_def.format.image.eColorFormat = OMX_COLOR_Format32bitARGB8888;
  port_def.format.image.nFrameWidth = width;
  port_def.format.image.nFrameHeight = height;
  port_def.format.image.nStride = pitch;
  port_def.format.image.nSliceHeight = (height+15) & ~15;
  port_def.format.image.bFlagErrorConcealment = OMX_FALSE;

  omx_err = m_omx_encoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  OMX_INIT_STRUCTURE(port_def);
  port_def.nPortIndex = m_omx_encoder.GetOutputPort();

  omx_err = m_omx_encoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.GetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  port_def.format.image.eCompressionFormat = OMX_IMAGE_CodingJPEG;
  port_def.format.image.eColorFormat = OMX_COLOR_FormatUnused;
  port_def.format.image.nFrameWidth = width;
  port_def.format.image.nFrameHeight = height;
  port_def.format.image.nStride = 0;
  port_def.format.image.nSliceHeight = 0;
  port_def.format.image.bFlagErrorConcealment = OMX_FALSE;

  omx_err = m_omx_encoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  OMX_IMAGE_PARAM_QFACTORTYPE qfactor;
  OMX_INIT_STRUCTURE(qfactor);
  qfactor.nPortIndex = m_omx_encoder.GetOutputPort();
  qfactor.nQFactor = 16;

  omx_err = m_omx_encoder.SetParameter(OMX_IndexParamQFactor, &qfactor);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.SetParameter OMX_IndexParamQFactor result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_encoder.AllocInputBuffers();
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.AllocInputBuffers result(0x%x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_encoder.AllocOutputBuffers();
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.AllocOutputBuffers result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_encoder.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.SetStateForComponent result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  demuxer_content = buffer;
  demuxer_bytes   = height * pitch;

  if(!demuxer_bytes || !demuxer_content)
    return false;

  while(demuxer_bytes > 0)
  {
    omx_buffer = m_omx_encoder.GetInputBuffer(1000);
    if(omx_buffer == NULL)
    {
      return false;
    }

    omx_buffer->nOffset = omx_buffer->nFlags  = 0;

    omx_buffer->nFilledLen = (demuxer_bytes > omx_buffer->nAllocLen) ? omx_buffer->nAllocLen : demuxer_bytes;
    memcpy(omx_buffer->pBuffer, demuxer_content, omx_buffer->nFilledLen);

    demuxer_content += omx_buffer->nFilledLen;
    demuxer_bytes -= omx_buffer->nFilledLen;

    if(demuxer_bytes == 0)
      omx_buffer->nFlags |= OMX_BUFFERFLAG_EOS;

    omx_err = m_omx_encoder.EmptyThisBuffer(omx_buffer);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
      break;
    }
  }

  m_encoded_buffer = m_omx_encoder.GetOutputBuffer();

  if(!m_encoded_buffer)
  {
    CLog::Log(LOGERROR, "%s::%s no output buffer\n", CLASSNAME, __func__);
    return false;
  }

  omx_err = m_omx_encoder.FillThisBuffer(m_encoded_buffer);
  if(omx_err != OMX_ErrorNone)
    return false;

  omx_err = m_omx_encoder.WaitForEvent(OMX_EventBufferFlag, 1000);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder WaitForEvent result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  m_encoded_format.nPortIndex = m_omx_encoder.GetOutputPort();
  omx_err = m_omx_encoder.GetParameter(OMX_IndexParamPortDefinition, &m_encoded_format);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_encoder.GetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return false;
  }

  if(m_omx_encoder.BadState())
    return false;

  return true;
}

unsigned char *COMXImage::GetDecodedData()
{
  if(!m_decoded_buffer)
    return NULL;

  return (unsigned char *)m_decoded_buffer->pBuffer;
}

unsigned int COMXImage::GetDecodedSize()
{
  if(!m_decoded_buffer)
    return 0;
  return (unsigned int)m_decoded_buffer->nFilledLen;
}

unsigned char *COMXImage::GetEncodedData()
{
  if(!m_encoded_buffer)
    return NULL;

  return (unsigned char *)m_encoded_buffer->pBuffer;
}

unsigned int COMXImage::GetEncodedSize()
{
  if(!m_encoded_buffer)
    return 0;
  return (unsigned int)m_encoded_buffer->nFilledLen;
}

bool COMXImage::SwapBlueRed(unsigned char *pixels, unsigned int height, unsigned int pitch, 
  unsigned int elements, unsigned int offset)
{
  if (!pixels) return false;
  unsigned char *dst = pixels;
  for (unsigned int y = 0; y < height; y++)
  {
    dst = pixels + (y * pitch);
    for (unsigned int x = 0; x < pitch; x+=elements)
      std::swap(dst[x+offset], dst[x+2+offset]);
  }
  return true;
}

bool COMXImage::CreateThumbnail(const CStdString& sourceFile, const CStdString& destFile, 
    int minx, int miny, bool rotateExif)
{
  if (!ReadFile(sourceFile))
    return false;

  return CreateThumbnailFromMemory(m_image_buffer, m_image_size, destFile, minx, miny);
}

bool COMXImage::CreateThumbnailFromMemory(unsigned char* buffer, unsigned int bufSize, const CStdString& destFile, 
    unsigned int minx, unsigned int miny)
{
  if(!bufSize || !buffer)
    return false;

  if(!m_is_open)
  {
    m_image_size = bufSize;
    m_image_buffer = (uint8_t *)malloc(m_image_size);
    if(!m_image_buffer)
      return false;

    memcpy(m_image_buffer, buffer, m_image_size);

    if(GetCodingType() != OMX_IMAGE_CodingJPEG) {
      CLog::Log(LOGERROR, "%s::%s : %s GetCodingType()=0x%x\n", CLASSNAME, __func__, destFile.c_str(), GetCodingType());
      return false;
    }
    m_is_open = true;
  }

  if(!Decode(minx, miny))
    return false;

  return CreateThumbnailFromSurface(GetDecodedData(), GetDecodedWidth(), GetDecodedHeight(), 
    XB_FMT_A8R8G8B8, GetDecodedStride(), destFile);
}

bool COMXImage::CreateThumbnailFromSurface(unsigned char* buffer, unsigned int width, unsigned int height, 
    unsigned int format, unsigned int pitch, const CStdString& destFile)
{
  if(format != XB_FMT_A8R8G8B8 || !buffer) {
    CLog::Log(LOGDEBUG, "%s::%s : %s failed format=0x%x\n", CLASSNAME, __func__, destFile.c_str(), format);
    return false;
  }

  if(!Encode(buffer, height * pitch, width, height, pitch)) {
    CLog::Log(LOGDEBUG, "%s::%s : %s encode failed\n", CLASSNAME, __func__, destFile.c_str());
    return false;
  }

  XFILE::CFile file;
  if (file.OpenForWrite(destFile, true))
  {
    CLog::Log(LOGDEBUG, "%s::%s : %s width %d height %d\n", CLASSNAME, __func__, destFile.c_str(), width, height);

    file.Write(GetEncodedData(), GetEncodedSize());
    file.Close();
    return true;
  }

  return false;
}
