/*
Mod Organizer archive handling

Copyright (C) 2012 Sebastian Herbord. All rights reserved.

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 3 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <Unknwn.h>
#include "archive.h"

#include "extractcallback.h"
#include "inputstream.h"
#include "opencallback.h"
#include "propertyvariant.h"

#include <QDebug>
#include <QDir>
#include <QLibrary>

#include <algorithm>
#include <map>
#include <stddef.h>
#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace PropID = NArchive::NHandlerPropID;

class FileDataImpl : public FileData {
  friend class Archive;
public:
  FileDataImpl(const QString &fileName, UInt64 crc, bool isDirectory);

  virtual QString getFileName() const;
  virtual void addOutputFileName(QString const &fileName);
  virtual std::vector<QString> getAndClearOutputFileNames();
  virtual bool isDirectory() const { return m_IsDirectory; }
  virtual uint64_t getCRC() const;

private:
  QString m_FileName;
  UInt64 m_CRC;
  std::vector<QString> m_OutputFileNames;
  bool m_IsDirectory;
};


QString FileDataImpl::getFileName() const
{
  return m_FileName;
}

void FileDataImpl::addOutputFileName(const QString &fileName)
{
  m_OutputFileNames.push_back(fileName);
}

std::vector<QString> FileDataImpl::getAndClearOutputFileNames()
{
  std::vector<QString> result = m_OutputFileNames;
  m_OutputFileNames.clear();
  return result;
}

uint64_t FileDataImpl::getCRC() const {
  return m_CRC;
}


FileDataImpl::FileDataImpl(const QString &fileName, UInt64 crc, bool isDirectory)
  : m_FileName(fileName)
  , m_CRC(crc)
  , m_IsDirectory(isDirectory)
{
}


/// represents the connection to one archive and provides common functionality
class ArchiveImpl : public Archive {

public:

  ArchiveImpl();
  virtual ~ArchiveImpl();

  virtual bool isValid() const { return m_Valid; }

  virtual Error getLastError() const { return m_LastError; }

  // The actual archive might start in the file on a non-zero offset
  virtual bool openWithOffset(QString const &archiveName, PasswordCallback *passwordCallback, UInt64 offset);
  virtual bool open(const QString &archiveName, PasswordCallback *passwordCallback);
  virtual void close();
  virtual bool getFileList(FileData* const *&data, size_t &size);
  virtual bool extract(QString const &outputDirectory, ProgressCallback *progressCallback,
                       FileChangeCallback* fileChangeCallback, ErrorCallback* errorCallback);

  virtual void cancel();

  void operator delete(void* ptr) {
    ::operator delete(ptr);
  }

private:

  virtual void destroy() {
    delete this;
  }

  void clearFileList();
  void resetFileList();

  HRESULT loadFormats();

private:

  typedef UINT32 (WINAPI *CreateObjectFunc)(const GUID *clsID, const GUID *interfaceID, void **outObject);
  CreateObjectFunc m_CreateObjectFunc;

  //A note: In 7zip source code this not is what this typedef is called, the old
  //GetHandlerPropertyFunc appears to be deprecated.
  typedef UInt32 (WINAPI *GetPropertyFunc)(UInt32 index, PROPID propID, PROPVARIANT *value);
  GetPropertyFunc m_GetHandlerPropertyFunc;

  template <typename T> T readHandlerProperty(UInt32 index, PROPID propID) const;

  template <typename T> T readProperty(UInt32 index, PROPID propID) const;

  bool m_Valid;
  Error m_LastError;

  QLibrary m_Library;
  QString m_ArchiveName; //TBH I don't think this is required
  CComPtr<IInArchive> m_ArchivePtr;
  CArchiveExtractCallback *m_ExtractCallback;

  PasswordCallback *m_PasswordCallback;

  std::vector<FileData*> m_FileList;

  QString m_Password;

  struct ArchiveFormatInfo
  {
    CLSID m_ClassID;
    std::wstring m_Name;
    std::vector<std::string> m_Signatures;
    std::wstring m_Extensions;
    std::wstring m_AdditionalExtensions;
    UInt32 m_SignatureOffset;
  };

  typedef std::vector<ArchiveFormatInfo> Formats;
  Formats m_Formats;

  typedef std::unordered_map<std::wstring, Formats> FormatMap;
  FormatMap m_FormatMap;

  //I don't think one signature could possibly describe two formats.
  typedef std::map<std::string, ArchiveFormatInfo> SignatureMap;
  SignatureMap m_SignatureMap;

  std::size_t m_MaxSignatureLen = 0;
};

template <typename T> T ArchiveImpl::readHandlerProperty(UInt32 index, PROPID propID) const
{
  PropertyVariant prop;
  if (m_GetHandlerPropertyFunc(index, propID, &prop) != S_OK) {
    throw std::runtime_error("Failed to read property");
  }
  return static_cast<T>(prop);
}

template <typename T> T ArchiveImpl::readProperty(UInt32 index, PROPID propID) const
{
  PropertyVariant prop;
  if (m_ArchivePtr->GetProperty(index, propID, &prop) != S_OK) {
    throw std::runtime_error("Failed to read property");
  }
  return static_cast<T>(prop);
}

//Seriously, there is one format returned in the list that has no registered
//extension and no signature. WTF?
HRESULT ArchiveImpl::loadFormats()
{
  typedef UInt32 (WINAPI *GetNumberOfFormatsFunc)(UInt32 *numFormats);
  GetNumberOfFormatsFunc getNumberOfFormats = reinterpret_cast<GetNumberOfFormatsFunc>(m_Library.resolve("GetNumberOfFormats"));
  if (getNumberOfFormats == nullptr) {
    return E_FAIL;
  }

  UInt32 numFormats;
  RINOK(getNumberOfFormats(&numFormats));

  for (UInt32 i = 0; i < numFormats; ++i)
  {
    ArchiveFormatInfo item;

    item.m_Name = readHandlerProperty<std::wstring>(i, PropID::kName);

    item.m_ClassID = readHandlerProperty<GUID>(i, PropID::kClassID);

    //Should split up the extensions and map extension to type, and see what we get from that for preference
    //then try all extensions anyway...
    item.m_Extensions = readHandlerProperty<std::wstring>(i, PropID::kExtension);

    //This is unnecessary currently for our purposes. Basically, for each
    //extension, there's an 'addext' which, if set (to other than *) means that
    //theres a double encoding going on. For instance, the bzip format is like this
    //addext = "* * .tar .tar"
    //ext    = "bz2 bzip2 tbz2 tbz"
    //which means that tbz2 and tbz should uncompress to a tar file which can be
    //further processed as if it were a tar file. Having said which, we don't
    //need to support this at all, so I'm storing it but ignoring it.
    item.m_AdditionalExtensions = readHandlerProperty<std::wstring>(i, PropID::kAddExtension);

    std::string signature = readHandlerProperty<std::string>(i, PropID::kSignature);
    if (!signature.empty()) {
      item.m_Signatures.push_back(signature);
      if (m_MaxSignatureLen < signature.size()) {
        m_MaxSignatureLen = signature.size();
      }
      m_SignatureMap[signature] = item;
    }

    std::string multiSig = readHandlerProperty<std::string>(i, PropID::kMultiSignature);
    const char *multiSigBytes = multiSig.c_str();
    unsigned size = multiSig.length();
    while (size > 0) {
      unsigned len = *multiSigBytes++;
      size--;
      if (len > size) break;
      std::string sig(multiSigBytes, multiSigBytes + len);
      multiSigBytes = multiSigBytes + len;
      size -= len;
      item.m_Signatures.push_back(sig);
      if (m_MaxSignatureLen < sig.size()) {
        m_MaxSignatureLen = sig.size();
      }
      m_SignatureMap[sig] = item;
    }

    UInt32 offset = readHandlerProperty<UInt32>(i, PropID::kSignatureOffset);
    item.m_SignatureOffset = offset;

    //Now split the extension up from the space separated string and create
    //a map from each extension to the possible formats
    //We could make these pointers but it's not a massive overhead and nobody
    //should be changing this
    std::wistringstream s(item.m_Extensions);
    std::wstring t;
    while (s >> t) {
      m_FormatMap[t].push_back(item);
    }
    m_Formats.push_back(item);
  }
  return S_OK;
}


ArchiveImpl::ArchiveImpl()
  : m_Valid(false)
  , m_LastError(ERROR_NONE)
  , m_Library("dlls/7z")
  , m_PasswordCallback(nullptr)
{
  if (!m_Library.load()) {
    m_LastError = ERROR_LIBRARY_NOT_FOUND;
    return;
  }

  m_CreateObjectFunc = reinterpret_cast<CreateObjectFunc>(m_Library.resolve("CreateObject"));
  if (m_CreateObjectFunc == nullptr) {
    m_LastError = ERROR_LIBRARY_INVALID;
    return;
  }

  m_GetHandlerPropertyFunc = reinterpret_cast<GetPropertyFunc>(m_Library.resolve("GetHandlerProperty2"));
  if (m_GetHandlerPropertyFunc == nullptr) {
    m_LastError = ERROR_LIBRARY_INVALID;
    return;
  }

  try {
    if (loadFormats() != S_OK) {
      m_LastError = ERROR_LIBRARY_INVALID;
      return;
    }

    m_Valid = true;
    return;
  }
  catch (std::exception const &e) {
    qDebug() << "Caught exception " << e.what();
    m_LastError = ERROR_LIBRARY_INVALID;
  }
}

ArchiveImpl::~ArchiveImpl()
{
  close();
}

bool ArchiveImpl::open(QString const &archiveName, PasswordCallback *passwordCallback) {
	QFileInfo finfo(archiveName);
	CComPtr<InputStream> file(new InputStream);

	if (!file->Open(finfo.absoluteFilePath())) {
		m_LastError = ERROR_FAILED_TO_OPEN_ARCHIVE;
		return false;
	}
	
	QString extension = finfo.suffix().toLower();
	if (extension == "png") {
		qDebug() << "Trying to open png file as an archive";
		// Ignoring PNG signature, seeking over it
		file->Seek(8, FILE_BEGIN, nullptr);
		UInt64 total_length = 8;
		
		// No safe length checks, no fallback on corrupt PNG files -- life is short, so is my code
		while (true) {
			// TODO better way to handle endianness
			UInt32 chunk_length;
			file->Read(&chunk_length, 4, nullptr);
			chunk_length = (((chunk_length>>24) & 0x000000ff) | ((chunk_length>>8) & 0x0000ff00) | ((chunk_length<<8) & 0x00ff0000) | ((chunk_length<<24) & 0xff000000));
			
			std::vector<char> buf;
			buf.reserve(4);
			file->Read(buf.data(), 4, nullptr);
			std::string chunk_type = std::string(buf.data(), 4);
			file->Seek(4 + chunk_length, FILE_CURRENT, nullptr); // 4 is for CRC
			total_length += 12 + chunk_length;
			if (chunk_type == "IEND") {
				break;
			}
		}
		if (openWithOffset(archiveName, passwordCallback, total_length)) {
			return true;
		} else {
			qDebug() << "Failed to extract archive from PNG";
		}
	}
	return openWithOffset(archiveName, passwordCallback, 0);
}

bool ArchiveImpl::openWithOffset(QString const &archiveName, PasswordCallback *passwordCallback, UInt64 offset)
{
  m_ArchiveName = archiveName; //Just for debugging, not actually used...

  Formats formatList = m_Formats;

  //I sincerely hope QFileInfo works with v. long. paths (although 7z original
  //doesn't either...)
  QFileInfo finfo(archiveName);

  //If it doesn't exist or is a directory, error
  if (!finfo.exists() || finfo.isDir()) {
    m_LastError = ERROR_ARCHIVE_NOT_FOUND;
    return false;
  }

  // in rars the password seems to be requested during extraction, not on open, so we need to hold on
  // to the callback for now
  m_PasswordCallback = passwordCallback;

  CComPtr<InputStream> file(new InputStream);

  if (!file->Open(finfo.absoluteFilePath())) {
    m_LastError = ERROR_FAILED_TO_OPEN_ARCHIVE;
    return false;
  }

  CComPtr<CArchiveOpenCallback> openCallbackPtr(new CArchiveOpenCallback(passwordCallback, finfo));

  // Try to open the archive

  bool sigMismatch = false;

  {
    //Get the first iterator that is strictly > the signature we're looking for.
    for (auto signatureInfo : m_SignatureMap) {
      //Read the signature of the file and look that up.
      std::vector<char> buff;
      buff.reserve(m_MaxSignatureLen);
      UInt32 act;
      file->Seek(offset, STREAM_SEEK_SET, nullptr); // Feels like m_SignatureOffset was missing here in the original code
      file->Read(buff.data(), static_cast<UInt32>(m_MaxSignatureLen), &act);
      file->Seek(offset, STREAM_SEEK_SET, nullptr);
      std::string signature = std::string(buff.data(), act);
      if (signatureInfo.first == std::string(buff.data(), signatureInfo.first.size())) {
        if (m_CreateObjectFunc(&signatureInfo.second.m_ClassID, &IID_IInArchive, (void**)&m_ArchivePtr) != S_OK) {
          m_LastError = ERROR_LIBRARY_ERROR;
          return false;
        }

        if (m_ArchivePtr->Open(file, 0, openCallbackPtr) != S_OK) {
          qDebug() << "Failed to open " << archiveName << " using " <<
            QString::fromStdWString(signatureInfo.second.m_Name) << " (from signature)";
          m_ArchivePtr.Release();
        }
        else {
          qDebug() << "Opened " << archiveName << " using " <<
            QString::fromStdWString(signatureInfo.second.m_Name) << " (from signature)";
          QString extension = finfo.suffix().toLower();
          std::wstring ext(extension.toStdWString());
          std::wistringstream s(signatureInfo.second.m_Extensions);
          std::wstring t;
          bool found = false;
          while (s >> t) {
            if (t == ext) {
              found = true;
              break;
            }
          }
          if (!found) {
            qWarning() << "WARNING: The extension of this file did not match the expected extensions for this format! You may want to inform the mod author.";
            sigMismatch = true;
          }
        }
        //Arguably we should give up here if it's not OK if 7zip can't even start
        //to decode even though we've found the format from the signature.
        //Sadly, the 7zip API documentation is pretty well non-existant.
        break;
      }
      std::vector<ArchiveFormatInfo>::iterator iter = std::find_if(formatList.begin(), formatList.end(), [=](ArchiveFormatInfo a) -> bool { return a.m_Name == signatureInfo.second.m_Name; });
      if (iter != formatList.end())
        formatList.erase(iter);
    }
  }

  {
    // determine archive type based on extension
    Formats const *formats = nullptr;
    QString extension = finfo.suffix().toLower();
    std::wstring ext(extension.toStdWString());
    FormatMap::const_iterator map_iter = m_FormatMap.find(ext);
    if (map_iter != m_FormatMap.end()) {
      formats = &map_iter->second;
      if (formats != nullptr) {
        if (m_ArchivePtr == nullptr) {
          //OK, we have some potential formats. If there is only one, try it now. If
          //there are multiple formats, we'll try by signature lookup first.
          for (ArchiveFormatInfo format : *formats) {
            if (m_CreateObjectFunc(&format.m_ClassID, &IID_IInArchive, (void**)&m_ArchivePtr) != S_OK) {
              m_LastError = ERROR_LIBRARY_ERROR;
              return false;
            }

            if (m_ArchivePtr->Open(file, 0, openCallbackPtr) != S_OK) {
              qDebug() << "Failed to open " << archiveName << " using " <<
                QString::fromStdWString(format.m_Name) << " (from extension)";
              m_ArchivePtr.Release();
            }
            else {
              qDebug() << "Opened " << archiveName << " using " <<
                QString::fromStdWString(format.m_Name) << " (from extension)";
              break;
            }

            std::vector<ArchiveFormatInfo>::iterator iter = std::find_if(formatList.begin(), formatList.end(), [=](ArchiveFormatInfo a) -> bool { return a.m_Name == format.m_Name; });
            if (iter != formatList.end())
              formatList.erase(iter);
          }
        } else if (sigMismatch) {
          QStringList formatList;
          for (ArchiveFormatInfo format : *formats)
            formatList.append(QString::fromStdWString(format.m_Name));
          qWarning() << "WARNING: The format(s) expected for this extension are: " << formatList.join(", ");
        }
      }
    }
  }

  if (m_ArchivePtr == nullptr) {
    qWarning() << "WARNING: We're trying to open an archive but could not recognize the extension or signature.";
    qDebug() << "Attempting to open the file with the remaining formats as a fallback...";
    for (auto format : formatList) {
      if (m_CreateObjectFunc(&format.m_ClassID, &IID_IInArchive, (void**)&m_ArchivePtr) != S_OK) {
        m_LastError = ERROR_LIBRARY_ERROR;
        return false;
      }
      if (m_ArchivePtr->Open(file, 0, openCallbackPtr) == S_OK) {
        qDebug() << "Opened " << archiveName << " using " <<
          QString::fromStdWString(format.m_Name) << " (scan fallback)";
        qWarning() << "NOTE: This archive likely has an incorrect extension. Please contact the mod author.";
        break;
      } else
        m_ArchivePtr.Release();
    }
  }

  if (m_ArchivePtr == nullptr) {
    m_LastError = ERROR_INVALID_ARCHIVE_FORMAT;
    return false;
  }

  m_Password = openCallbackPtr->GetPassword();
/*
  UInt32 subFile = ULONG_MAX;
  {
    NCOM::CPropVariant prop;
    if (m_ArchivePtr->GetArchiveProperty(kpidMainSubfile, &prop) != S_OK) {
      throw std::runtime_error("failed to get property kpidMainSubfile");
    }

    if (prop.vt == VT_UI4) {
      subFile = prop.ulVal;
    }
  }

  if (subFile != ULONG_MAX) {
    std::wstring subPath = GetArchiveItemPath(m_ArchivePtr, subFile);

    CMyComPtr<IArchiveOpenSetSubArchiveName> setSubArchiveName;
    openCallbackPtr.QueryInterface(IID_IArchiveOpenSetSubArchiveName, (void **)&setSubArchiveName);
    if (setSubArchiveName) {
      setSubArchiveName->SetSubArchiveName(subPath.c_str());
    }
  }*/

  m_LastError = ERROR_NONE;

  resetFileList();
  return true;
}


void ArchiveImpl::close()
{
  if (m_ArchivePtr != nullptr) {
    m_ArchivePtr->Close();
  }
  clearFileList();
  m_ArchivePtr.Release();
  delete m_PasswordCallback;
  m_PasswordCallback = nullptr;
}


void ArchiveImpl::clearFileList()
{
  for (std::vector<FileData*>::iterator iter = m_FileList.begin(); iter != m_FileList.end(); ++iter) {
    delete *iter;
  }
  m_FileList.clear();
}

void ArchiveImpl::resetFileList()
{
  UInt32 numItems = 0;
  clearFileList();

  m_ArchivePtr->GetNumberOfItems(&numItems);

  for (UInt32 i = 0; i < numItems; ++i) {
    m_FileList.push_back(new FileDataImpl(readProperty<QString>(i, kpidPath),
                                          readProperty<UInt64>(i, kpidCRC),
                                          readProperty<bool>(i, kpidIsDir)));
  }
}


bool ArchiveImpl::getFileList(FileData* const *&data, size_t &size)
{
  data = &m_FileList[0];
  size = m_FileList.size();
  return true;
}


bool ArchiveImpl::extract(const QString &outputDirectory, ProgressCallback* progressCallback,
                          FileChangeCallback* fileChangeCallback, ErrorCallback* errorCallback)
{
  m_ExtractCallback = new CArchiveExtractCallback(progressCallback,
                                                  fileChangeCallback,
                                                  errorCallback,
                                                  m_PasswordCallback,
                                                  m_ArchivePtr,
                                                  outputDirectory,
                                                  &m_FileList[0],
                                                  &m_Password);
  HRESULT result = m_ArchivePtr->Extract(nullptr, (UInt32)(Int32)(-1), false, m_ExtractCallback);
  //Note: m_ExtractCallBack is deleted by Extract
  switch (result) {
    case S_OK: {
      //nop
    } break;
    case E_ABORT: {
      m_LastError = ERROR_EXTRACT_CANCELLED;
    } break;
    case E_OUTOFMEMORY: {
      m_LastError = ERROR_OUT_OF_MEMORY;
    } break;
    default: {
      m_LastError = ERROR_LIBRARY_ERROR;
    } break;
  }

  return result == S_OK;
}


void ArchiveImpl::cancel()
{
  m_ExtractCallback->SetCanceled(true);
}


extern "C" DLLEXPORT Archive *CreateArchive()
{
  return new ArchiveImpl;
}
