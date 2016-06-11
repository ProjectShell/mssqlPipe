#include "stdafx.h"
#include "vdi/vdiguid.h"

#include "util.h"
#include "params.h"

#include "pipestat.h"

/****/

constexpr auto mssqlPipe_Version = L"1.2.0";

/****/

std::mutex outputMutex_;

/****/

_COM_SMARTPTR_TYPEDEF(IClientVirtualDeviceSet2, __uuidof(IClientVirtualDeviceSet2));
_COM_SMARTPTR_TYPEDEF(IClientVirtualDevice, __uuidof(IClientVirtualDevice));

/****/

struct InputFile
{
	InputFile(HANDLE hFile, size_t buflen)
		: hFile(hFile)
		, membufReserved(buflen)
	{
		if (membufReserved) {			
			membuf.reset(new BYTE[membufReserved]);
		}
	}

	void fillBuffer()
	{
		if (!membuf) {
			return;
		}
	}

	HRESULT resetPos()
	{
		if (streamPos > membufLen) {
			return E_FAIL;
		}

		membufPos = 0;
		return S_OK;
	}

	DWORD read(BYTE* buf, DWORD len)
	{
		DWORD totalRead = 0;
		if (membuf) {
			// if first read, fill the buffer
			if (membufLen == 0 && membufPos == 0 && streamPos == 0 && membufReserved != 0) {
				DWORD bytesRead = 0;

				while (bytesRead < membufReserved) {
					DWORD dwBytes = 0;
					BOOL ret = ::ReadFile(hFile, membuf.get() + bytesRead, membufReserved - bytesRead, &dwBytes, nullptr);
					bytesRead += dwBytes;
					if (!ret || (dwBytes == 0)) {
						break;
					}
				}

				streamPos += bytesRead;

				membufLen = bytesRead;
				membufPos = 0;
			}

			DWORD membufAvail = membufLen - membufPos;

			if (membufAvail) {
				DWORD bytesToRead = len;
				if (bytesToRead > membufAvail) {
					bytesToRead = membufAvail;
				}
				memcpy(buf, membuf.get() + membufPos, bytesToRead);
				membufPos += bytesToRead;
				totalRead += bytesToRead;

				len -= bytesToRead;
				buf += bytesToRead;
			}
			else {
				membuf.reset();
			}
		}

		DWORD streamRead = 0;

		while (streamRead < len) {
			DWORD dwBytes = 0;
			BOOL ret = ::ReadFile(hFile, buf, len, &dwBytes, nullptr);
			streamRead += dwBytes;
			if (!ret || (dwBytes == 0)) {
				break;
			}
		}

		streamPos += streamRead;
		totalRead += streamRead;

		return totalRead;
	}

protected:
	HANDLE hFile = nullptr;
	std::unique_ptr<BYTE[]> membuf;
	size_t membufReserved = 0;
	size_t membufLen = 0;
	size_t membufPos = 0;
	size_t streamPos = 0;
};

struct OutputFile
{
	explicit OutputFile(HANDLE hFile)
		: hFile(hFile)
	{		
	}

	DWORD write(void* buf, DWORD len)
	{
		DWORD dwBytesWritten = 0;
		while (dwBytesWritten < len) {
			DWORD dwBytes = 0;
			BOOL ret = ::WriteFile(hFile, static_cast<BYTE*>(buf) + dwBytesWritten, len - dwBytesWritten, &dwBytes, nullptr);
			dwBytesWritten += dwBytes;
			if (!ret || (dwBytes == 0)) {
				break;
			}
		}
		return dwBytesWritten;
	}

	void flush()
	{
		// not necessary with the win32 streams
		//fflush(file);
	}

protected:
	HANDLE hFile = nullptr;
};

/****/

HRESULT processPipeRestore(IClientVirtualDevice* pDevice, InputFile& file, bool quiet = false)
{
	if (!pDevice) {
		return E_INVALIDARG;
	}

	if (!quiet) {			
		std::unique_lock<std::mutex> lock(outputMutex_);
		std::wcerr << L"\nProcessing... " << std::endl;
	}

	pipestat ps(outputMutex_, quiet);

	static const DWORD timeout = 10 * 60 * 1000;
	HRESULT hr = S_OK;

	for (;;) {
		VDC_Command* pCmd = nullptr;		
		DWORD completionCode = 0;
		DWORD bytesTransferred = 0;

		/*If a client is using asynchronous I/O, it must ensure that a mechanism exists to complete outstanding requests when it is blocked in a 
		IClientVirtualDevice::GetCommand call. Because GetCommand waits in an Alertable state, that use of Alertable I/O is one such technique. 
		In this case, the operating system calls back to a completion routine set up by the client.*/
		hr = pDevice->GetCommand(timeout, &pCmd);

		if (!SUCCEEDED(hr)) {
			switch (hr) {
				case VD_E_CLOSE:
					// normal, we closed.
					hr = 0;
					break;
				case VD_E_TIMEOUT:
					break;
				case VD_E_ABORT:
					break;
				default:
					break;
			}
			break; // exit loop
		}

		switch (pCmd->commandCode) {
		case VDC_Read:
			while (bytesTransferred < pCmd->size) {
				bytesTransferred += file.read(pCmd->buffer + bytesTransferred, pCmd->size - bytesTransferred);
			}

			break;
		case VDC_Write:
			completionCode = ERROR_NOT_SUPPORTED;
			break;
		case VDC_Flush:
			completionCode = ERROR_NOT_SUPPORTED;
			break;
		case VDC_ClearError:
			break;
		default:
			completionCode = ERROR_NOT_SUPPORTED;
			break;
		}

		hr = pDevice->CompleteCommand(pCmd, completionCode, bytesTransferred, 0);

		ps.accumulate(bytesTransferred);

		if (!SUCCEEDED(hr)) {
			// error message?
			break;
		}
	}

	ps.finalize();
	
	return hr;
}

HRESULT processPipeBackup(IClientVirtualDevice* pDevice, OutputFile& file, bool quiet = false)
{
	if (!pDevice) {
		return E_INVALIDARG;
	}

	if (!quiet) {			
		std::unique_lock<std::mutex> lock(outputMutex_);
		std::wcerr << L"\nProcessing... " << std::endl;
	}

	pipestat ps(outputMutex_, quiet);

	static const DWORD timeout = 10 * 60 * 1000;
	HRESULT hr = S_OK;
	
	for (;;) {
		VDC_Command* pCmd = nullptr;		
		DWORD completionCode = 0;
		DWORD bytesTransferred = 0;

		/*If a client is using asynchronous I/O, it must ensure that a mechanism exists to complete outstanding requests when it is blocked in a 
		IClientVirtualDevice::GetCommand call. Because GetCommand waits in an Alertable state, that use of Alertable I/O is one such technique. 
		In this case, the operating system calls back to a completion routine set up by the client.*/
		hr = pDevice->GetCommand(timeout, &pCmd);

		if (!SUCCEEDED(hr)) {
			switch (hr) {
				case VD_E_CLOSE:
					// normal, we closed.
					hr = 0;
					break;
				case VD_E_TIMEOUT:
					break;
				case VD_E_ABORT:
					break;
				default:
					break;
			}
			break; // exit loop
		}

		switch (pCmd->commandCode) {
		case VDC_Read:
			completionCode = ERROR_NOT_SUPPORTED;
			break;
		case VDC_Write:
			while (bytesTransferred < pCmd->size) {
				bytesTransferred += file.write(pCmd->buffer + bytesTransferred, pCmd->size - bytesTransferred);
			}
			break;
		case VDC_Flush:
			file.flush();
			break;
		case VDC_ClearError:
			break;
		default:
			completionCode = ERROR_NOT_SUPPORTED;
			break;
		}

		hr = pDevice->CompleteCommand(pCmd, completionCode, bytesTransferred, 0);

		if (!SUCCEEDED(hr)) {
			// error message?
			break;
		}

		ps.accumulate(bytesTransferred);
	}

	ps.finalize();
	
	return hr;
}

/****/

struct VirtualDevice
{
	IClientVirtualDeviceSet2Ptr pSet;
	IClientVirtualDevicePtr pDevice;
		
	VDConfig config;
	
	std::wstring instance;
	std::wstring name;

	VirtualDevice(std::wstring instance, std::wstring name)
		: instance(std::move(instance))
		, name(std::move(name))
		, config({ 1 })
	{}

	~VirtualDevice()
	{
		Close();
	}

	HRESULT Close()
	{
		HRESULT hr = 0;
		if (pSet) {			
			hr = pSet->Close();
		}
		pDevice = nullptr;
		pSet = nullptr;
		return hr;
	}

	HRESULT Abort()
	{
		if (!pSet) {
			return S_FALSE;
		}
		return pSet->SignalAbort();
	}
		
	HRESULT Create()
	{
		HRESULT hr = S_OK;
	
		hr = pSet.CreateInstance(CLSID_MSSQL_ClientVirtualDeviceSet);
		if (!SUCCEEDED(hr)) {
			std::unique_lock<std::mutex> lock(outputMutex_);
			std::wcerr << L"Failed to cocreate device set: " << std::hex << hr << std::dec << std::endl;
			return hr;
		}

		const wchar_t* wInstance = instance.empty() ? nullptr : (const wchar_t*)instance.c_str();

		hr = pSet->CreateEx(wInstance, name.c_str(), &config);
		if (!SUCCEEDED(hr)) {
			std::unique_lock<std::mutex> lock(outputMutex_);
			std::wcerr << L"Failed to create device set: " << std::hex << hr << std::dec << std::endl;
			return hr;
		}
		else {			
		}

		return hr;
	}

	HRESULT Open(DWORD dwTimeout)
	{
		HRESULT hr = 0;

		hr = pSet->GetConfiguration(dwTimeout, &config);
		if (!SUCCEEDED(hr)) {
			pSet->Close();
			std::unique_lock<std::mutex> lock(outputMutex_);
			std::wcerr << L"Failed to initialize backup operation: " << std::hex << hr << std::dec << std::endl;
			return hr;
		}

		hr = pSet->OpenDevice(name.c_str(), &pDevice);
		if (!SUCCEEDED(hr)) {
			pSet->Close();
			std::unique_lock<std::mutex> lock(outputMutex_);
			std::wcerr << L"Failed to open backup device: " << std::hex << hr << std::dec << std::endl;
			return hr;
		}

		return hr;
	}
};

/****/

std::wstring EscapeConnectionStringValue(const std::wstring& val)
{
	auto pos = val.begin();

	bool hasSpace = false;
	int singleCount = 0;
	int doubleCount = 0;
	int otherCount = 0;

	if (!val.empty()) {
		if ((::iswspace(val.front())) || (::iswspace(val.back()))) {
			hasSpace = true;
		}
	}
	while (pos < val.end()) {
		switch (*pos++) {
		case L';':
			++otherCount;
			break;
		case L'\'':
			++singleCount;
			break;
		case L'\"':
			++doubleCount;
			break;
		}
	}

	int totalCount = otherCount + singleCount + doubleCount;

	if (!totalCount && !hasSpace) {
		return val;
	}

	std::wstring esc;
	esc.reserve(val.length() + 2 + (totalCount * 2));

	wchar_t c = L'\'';
	if (singleCount && !doubleCount) {
		c = L'\"';
	}
	esc.push_back(c);

	pos = val.begin();
	while (pos < val.end()) {
		esc.push_back(*pos);
		if (*pos == c) {
			esc.push_back(c);
		}
		++pos;
	}

	esc.push_back(c);

	return esc;
}

std::wstring MakeConnectionString(const std::wstring& instance, const std::wstring& username, const std::wstring& password)
{
	std::wstring connectionString;

	connectionString.reserve(256);
		
	// SQLNCLI might be a better option, but SQLOLEDB is ubiquitous. 
	connectionString = L"Provider=SQLOLEDB;Initial Catalog=master;";
	
	if (username.empty()) {
		connectionString += L"Integrated Security=SSPI;";
	}
	else {
		connectionString += L"User ID=";
		connectionString += EscapeConnectionStringValue(username);
		connectionString += L";Password=";
		connectionString += EscapeConnectionStringValue(password);
		connectionString += L";";
	}

	connectionString += L"Data Source=";

	std::wstring dataSource = L"lpc:.";
	if (!instance.empty()) {
		dataSource += L"\\";
		dataSource += instance;
	}

	connectionString += EscapeConnectionStringValue(dataSource);
	connectionString += L";";

	return connectionString;
}

ADODB::_ConnectionPtr Connect(std::wstring connectionString)
{
	try {
		ADODB::_ConnectionPtr pCon(__uuidof(ADODB::Connection));

		pCon->Open(connectionString.c_str(), L"", L"", ADODB::adConnectUnspecified);

		pCon->CommandTimeout = 0;

		pCon->CursorLocation = ADODB::adUseServer;

		return pCon;
	}
	catch (_com_error& e) {
		std::unique_lock<std::mutex> lock(outputMutex_);
		std::wcerr << L"Could not connect to " << connectionString << std::endl;
		std::wcerr << std::hex << e.Error() << std::dec << L": " << e.ErrorMessage() << std::endl;
		std::wcerr << e.Description() << std::endl;

		return nullptr;
	}
}

void traceAdoErrors(ADODB::_Connection* pCon) 
{
	auto errors = pCon->Errors;
	if (!errors) {
		return;
	}

	long count = errors->Count;
	if (!count) {
		return;
	}

	{
		std::unique_lock<std::mutex> lock(outputMutex_);
		for (long i = 0; i < count; ++i) {
			auto error = errors->Item[i];
			if (!error) {
				continue;
			}
			std::wcerr
				<< error->Description
				<< L"\t" << std::hex << error->Number << std::dec
				<< L"\t" << error->NativeError
				<< L"\t" << error->SQLState
				<< std::endl;
			//std::wcerr 
			//	<< L"[" << i << L"]"
			//	<< L"\t" << std::hex << error->Number << std::dec
			//	<< L"\t" << error->NativeError
			//	<< L"\t" << error->SQLState
			//	<< L"\t" << error->Description
			//	<< L"\t" << error->Source
			//	<< std::endl;
		}
	}

	errors->Clear();
}

void traceAdoRecordset(ADODB::_Recordset* pRs)
{
	if (!pRs->State || pRs->eof) {
		return;
	}

	auto fields = pRs->Fields;
	if (!fields) {
		return;
	}
	long count = fields->Count;

	std::unique_lock<std::mutex> lock(outputMutex_);
	for (long x = 0; x < count; ++x) {
		auto field = fields->Item[x];
		if (!field) {
			continue;
		}
		auto name = field->Name;
		if (!name.length()) {
			std::wcerr << L"\t[" << x << L"]";
		}
		else {
			std::wcerr << L"\t[" << field->Name << L"]";
		}
	}
	std::wcerr << std::endl;

	while (!pRs->eof) {
		for (long x = 0; x < count; ++x) {
			_variant_t var = pRs->Collect[x];
			HRESULT hrChange = 0;
			if (var.vt & VT_ARRAY) {
				hrChange = E_FAIL;
			}
			else {
				hrChange = ::VariantChangeType(&var, &var, 0, VT_BSTR);
			}

			if (SUCCEEDED(hrChange) && var.vt == VT_BSTR) {
				std::wcerr << L"\t" << var.bstrVal;
			}
			else if (var.vt == VT_NULL) {
				std::wcerr << L"\t";
			}
			else {
				std::wcerr << L"\t?";
			}
		}
		std::wcerr << std::endl;
		pRs->MoveNext();
	}
}

/****/

struct DbFile
{
	std::wstring logicalName;
	std::wstring physicalName;
	std::wstring type;
};

HRESULT RunPrepareRestoreDatabase(VirtualDevice& vd, params p, InputFile& inputFile, std::wstring& dataPath, std::wstring& logPath, std::vector<DbFile>& fileList, bool quiet)
{
	HRESULT hr = 0;

	std::wstring connectionString = MakeConnectionString(p.instance, p.username, p.password);

	std::wstring sql;
	{
		std::wostringstream o;
		o << L"restore filelistonly from virtual_device=N'" << escape(p.device) << L"';";
		sql = o.str();
	}

	auto pipeResult = std::async([&vd, &inputFile, &p, quiet]{
		CoInit comInit;

		vd.Open(p.timeout);
		return processPipeRestore(vd.pDevice, inputFile, quiet);
	});

	auto adoResult = std::async([&connectionString, &sql, &fileList]{
		CoInit comInit;
		
		try {
			ADODB::_ConnectionPtr pCon = Connect(connectionString);
			if (!pCon) {
				return E_FAIL;
			}

			ADODB::_RecordsetPtr pRs(__uuidof(ADODB::Recordset));
			pRs->CursorLocation = ADODB::adUseServer;
			pRs->Open(sql.c_str(), (IDispatch*)pCon, ADODB::adOpenForwardOnly, ADODB::adLockReadOnly, ADODB::adCmdText);

			traceAdoErrors(pCon);

			for (; pRs && !pRs->eof; pRs->MoveNext()) {
				DbFile f;
				f.logicalName = pRs->Fields->Item[L"LogicalName"]->Value.bstrVal;
				f.physicalName = pRs->Fields->Item[L"PhysicalName"]->Value.bstrVal;
				f.type = pRs->Fields->Item[L"Type"]->Value.bstrVal;

				fileList.push_back(f);
			}
			traceAdoErrors(pCon);

			pCon->Close();

			return S_OK;
		}
		catch (_com_error& e) {			
			std::unique_lock<std::mutex> lock(outputMutex_);
			std::wcerr << std::hex << e.Error() << std::dec << L": " << e.ErrorMessage() << std::endl;
			std::wcerr << e.Description() << std::endl;
			return e.Error();
		}
	});
	
	auto adoPathsResult = std::async([&connectionString, &dataPath, &logPath]{
		CoInit comInit;
		
		try {
			ADODB::_ConnectionPtr pCon = Connect(connectionString);
			if (!pCon) {
				return E_FAIL;
			}
			
			// InstanceDefaultDataPath and InstanceDefaultLogPath are SQL2012+
			auto query = LR"(
;with database_info as (
select 
	substring(physical_name, 1, len(physical_name) - charindex('\', reverse(physical_name))) as physical_path
	, case when database_id in (db_id('master'), db_id('msdb'), db_id('tempdb'), db_id('model')) then 1 else 0 end as is_system
	, type
from sys.master_files
)
select 
	coalesce( convert(nvarchar(512), serverproperty('InstanceDefaultDataPath')), (
		select top 1 physical_path from database_info
		where type = 0
		group by physical_path, is_system
		order by is_system, count(*), physical_path
	)) as DefaultData
	, 
	coalesce( convert(nvarchar(512), serverproperty('InstanceDefaultLogPath')), (
		select top 1 physical_path from database_info
		where type = 1
		group by physical_path, is_system
		order by is_system, count(*), physical_path
	)) as DefaultLog
;
)";

			ADODB::_RecordsetPtr pRs(__uuidof(ADODB::Recordset));
			pRs->CursorLocation = ADODB::adUseServer;
			pRs->Open(query, (IDispatch*)pCon, ADODB::adOpenForwardOnly, ADODB::adLockReadOnly, ADODB::adCmdText);

			traceAdoErrors(pCon);

			if (!pRs->eof) {
				
				dataPath = pRs->Fields->Item[L"DefaultData"]->Value.bstrVal;
				logPath = pRs->Fields->Item[L"DefaultLog"]->Value.bstrVal;

				traceAdoErrors(pCon);
			}

			pCon->Close();

			return S_OK;
		}
		catch (_com_error& e) {			
			std::unique_lock<std::mutex> lock(outputMutex_);
			std::wcerr << std::hex << e.Error() << std::dec << L": " << e.ErrorMessage() << std::endl;
			std::wcerr << e.Description() << std::endl;
			return e.Error();
		}
	});

	HRESULT hrAdoPaths = adoPathsResult.get();

	if (!SUCCEEDED(hrAdoPaths)) {
		if (!hr) {
			hr = hrAdoPaths;
		}
		vd.Abort();
	}

	HRESULT hrAdo = adoResult.get();

	if (!SUCCEEDED(hrAdo)) {
		if (!hr) {
			hr = hrAdo;
		}
		vd.Abort();
	}

	HRESULT hrPipe = pipeResult.get();
	if (!SUCCEEDED(hrPipe)) {
		if (!hr) {
			hr = hrAdo;
		}
	}

	return hr;
}

HRESULT RunRestoreDatabase(VirtualDevice& vd, params p, std::wstring sql, InputFile& inputFile, bool quiet)
{
	HRESULT hr = 0;

	std::wstring connectionString = MakeConnectionString(p.instance, p.username, p.password);

	if (!quiet) {
		std::unique_lock<std::mutex> lock(outputMutex_);
		std::wcerr << L"Restoring via virtual device " << p.device << std::endl;
	}
	
	auto pipeResult = std::async([&vd, &inputFile, &p, quiet]{
		CoInit comInit;

		vd.Open(p.timeout);
		return processPipeRestore(vd.pDevice, inputFile, quiet);
	});

	auto adoResult = std::async([&connectionString, &sql]{
		CoInit comInit;
		
		try {
			ADODB::_ConnectionPtr pCon = Connect(connectionString);
			if (!pCon) {
				return E_FAIL;
			}

			ADODB::_RecordsetPtr pRs(__uuidof(ADODB::Recordset));
			pRs->CursorLocation = ADODB::adUseServer;
			pRs->Open(sql.c_str(), (IDispatch*)pCon, ADODB::adOpenForwardOnly, ADODB::adLockReadOnly, ADODB::adCmdText);

			traceAdoErrors(pCon);

			while (pRs) {
				traceAdoRecordset(pRs);

				traceAdoErrors(pCon);
				_variant_t varAffected;
				pRs = pRs->NextRecordset(&varAffected);

				traceAdoErrors(pCon);
			}

			pCon->Close();

			return S_OK;
		}
		catch (_com_error& e) {			
			std::unique_lock<std::mutex> lock(outputMutex_);
			std::wcerr << std::hex << e.Error() << std::dec << L": " << e.ErrorMessage() << std::endl;
			std::wcerr << e.Description() << std::endl;
			return e.Error();
		}
	});

	HRESULT hrAdo = adoResult.get();

	if (!SUCCEEDED(hrAdo)) {
		if (!hr) {
			hr = hrAdo;
		}
		vd.Abort();
	}

	HRESULT hrPipe = pipeResult.get();
	if (!SUCCEEDED(hrPipe)) {
		if (!hr) {
			hr = hrAdo;
		}
	}

	return hr;
}

std::wstring BuildRestoreCommand(params p, std::wstring dataPath, std::wstring logPath, const std::vector<DbFile>& fileList)
{
	if (!p.to.empty()) {
		dataPath = p.to;
		logPath = p.to;
	}

	if (!dataPath.empty() && dataPath.back() != L'\\') {
		dataPath += L'\\';
	}
	if (!logPath.empty() && logPath.back() != L'\\') {
		logPath += L'\\';
	}

	std::wostringstream o;
	o << L"restore database [" << escape(p.database) << L"] from virtual_device=N'" << escape(p.device) << L"' with ";
	
	if (iequals(p.subcommand, L"replace")) {
		o << L"replace, ";
	}

	// build moves?
	if (!fileList.empty()) {
		std::set<std::wstring, iless_predicate> fileRoots;
		for (auto file : fileList) {
			std::wstring targetBase;
			if (iequals(file.type, L"D")) {
				targetBase = dataPath + p.database + L"_dat";
			}
			else {
				targetBase = logPath + p.database + L"_log";
			}
			
			std::wstring target = targetBase;
			size_t ordinal = 1;
			while (fileRoots.count(target)) {
				++ordinal;
				std::wostringstream newTarget;
				newTarget << target << ordinal;
				target = newTarget.str();
			}
			fileRoots.insert(target);
						
			if (iequals(file.type, L"D")) {
				target += L".mdf";
			}
			else {
				target += L".ldf";
			}

			o << L"move N'" << escape(file.logicalName) << L"' to N'" << escape(target) << L"', ";
		}
	}
		
	o << L"nounload"; // noop basically, so i dont have to deal with trailing comma

	o << L";";
	
	return o.str();
}

HRESULT RunRestore(VirtualDevice& vd, params p, HANDLE hFile)
{
	HRESULT hr = S_OK;

	InputFile inputFile(hFile, 0x10000);

	if (iequals(p.subcommand, L"filelistonly")) {
		
		std::wostringstream o;
		o << L"restore filelistonly from virtual_device=N'" << escape(p.device) << L"';";

		hr = RunRestoreDatabase(vd, p, o.str(), inputFile, true);
	
		if (!SUCCEEDED(hr)) {
			std::unique_lock<std::mutex> lock(outputMutex_);
			std::wcerr << L"RunRestoreDatabase generic failed with " << std::hex << hr << std::dec << std::endl;
			return hr;
		}

		return hr;
	}

	std::vector<DbFile> fileList;
	std::wstring dataPath;
	std::wstring logPath;

	{
		params altp = p;
		altp.device = make_guid();

		VirtualDevice altvd(altp.instance, altp.device);
		hr = altvd.Create();
		if (!SUCCEEDED(hr)) {
			return hr;
		}

		hr = RunPrepareRestoreDatabase(altvd, altp, inputFile, dataPath, logPath, fileList, true);
		if (!SUCCEEDED(hr)) {
			std::unique_lock<std::mutex> lock(outputMutex_);
			std::wcerr << L"RunRestoreFileListOnly failed with " << std::hex << hr << std::dec << std::endl;
			return hr;
		}
	}

	hr = inputFile.resetPos();
	if (!SUCCEEDED(hr)) {
		std::unique_lock<std::mutex> lock(outputMutex_);
		std::wcerr << L"RunRestore failed to reset buffered input pos with " << std::hex << hr << std::dec << std::endl;
		return hr;
	}
	
	auto sql = BuildRestoreCommand(p, dataPath, logPath, fileList);

	hr = RunRestoreDatabase(vd, p, sql, inputFile, false);
	
	if (!SUCCEEDED(hr)) {
		std::unique_lock<std::mutex> lock(outputMutex_);
		std::wcerr << L"RunRestoreDatabase failed with " << std::hex << hr << std::dec << std::endl;
		return hr;
	}

	return hr;
}

HRESULT RunBackup(VirtualDevice& vd, params p, HANDLE hFile)
{
	HRESULT hr = 0;

	std::wstring connectionString = MakeConnectionString(p.instance, p.username, p.password);

	std::wstring sql;
	{
		std::wostringstream o;
		// always do copy_only, could be an option in the future
		o << L"backup database [" << escape(p.database) << L"] to virtual_device=N'" << escape(p.device) << L"' with copy_only;";
		sql = o.str();
	}

	{
		std::unique_lock<std::mutex> lock(outputMutex_);
		std::wcerr << L"Backing up via virtual device " << p.device << std::endl;
	}

	OutputFile outputFile(hFile);

	auto pipeResult = std::async([&vd, &outputFile, &p]{
		CoInit comInit;

		vd.Open(p.timeout);
		return processPipeBackup(vd.pDevice, outputFile);
	});

	auto adoResult = std::async([&connectionString, &sql]{
		CoInit comInit;
		
		try {
			ADODB::_ConnectionPtr pCon = Connect(connectionString);
			if (!pCon) {
				return E_FAIL;
			}

			ADODB::_RecordsetPtr pRs(__uuidof(ADODB::Recordset));
			pRs->CursorLocation = ADODB::adUseServer;
			pRs->Open(sql.c_str(), (IDispatch*)pCon, ADODB::adOpenForwardOnly, ADODB::adLockReadOnly, ADODB::adCmdText);

			traceAdoErrors(pCon);

			while (pRs) {
				traceAdoRecordset(pRs);

				traceAdoErrors(pCon);
				_variant_t varAffected;
				pRs = pRs->NextRecordset(&varAffected);

				traceAdoErrors(pCon);
			}

			pCon->Close();

			return S_OK;
		}
		catch (_com_error& e) {			
			std::unique_lock<std::mutex> lock(outputMutex_);
			std::wcerr << std::hex << e.Error() << std::dec << L": " << e.ErrorMessage() << std::endl;
			std::wcerr << e.Description() << std::endl;
			return e.Error();
		}
	});

	HRESULT hrAdo = adoResult.get();

	if (!SUCCEEDED(hrAdo)) {
		if (!hr) {
			hr = hrAdo;
		}
		vd.Abort();
	}

	HRESULT hrPipe = pipeResult.get();
	if (!SUCCEEDED(hrPipe)) {
		if (!hr) {
			hr = hrAdo;
		}
	}

	return hr;
}

HRESULT RunPipe(VirtualDevice& vd, params p, HANDLE hFile)
{
	HRESULT hr = 0;

	{
		std::unique_lock<std::mutex> lock(outputMutex_);
		std::wcerr << L"Piping " << p.subcommand << L" virtual device " << p.device << std::endl;
	}

	// in pipe mode, use a much larger than the default timeout, say 5 minutes
	DWORD pipeTimeout = 5 * 60 * 1000;

	if (iequals(p.subcommand, L"from")) {
		OutputFile outputFile(hFile);

		auto pipeResult = std::async([&vd, &outputFile, pipeTimeout]{
			CoInit comInit;

			vd.Open(pipeTimeout);
			return processPipeBackup(vd.pDevice, outputFile);
		});

		hr = pipeResult.get();
	}
	else if (iequals(p.subcommand, L"to")) {

		InputFile inputFile(hFile, 0); // no buffering

		auto pipeResult = std::async([&vd, &inputFile, pipeTimeout]{
			CoInit comInit;

			vd.Open(pipeTimeout);
			return processPipeRestore(vd.pDevice, inputFile);
		});

		hr = pipeResult.get();
	}

	return hr;
}	

HRESULT Elevate(params p, HANDLE hInput, HANDLE hOutput, std::wstring namedPipe, std::wstring stderrPipe)
{
	if (!hInput && !hOutput) {
		std::wcerr << L"Invalid command for elevation" << std::endl;
		return E_FAIL;
	}
	
	HANDLE hPipe = ::CreateNamedPipe(namedPipe.c_str()
		, hOutput ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE
		, PIPE_TYPE_BYTE | PIPE_WAIT
		, 1
		, 0x10000
		, 0x10000
		, 10000
		, nullptr
	);

	HANDLE hStderrPipe = ::CreateNamedPipe(stderrPipe.c_str()
		, PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE
		, PIPE_TYPE_BYTE | PIPE_WAIT
		, 1
		, 0x4000
		, 0x4000
		, 10000
		, nullptr
	);

	// launch
	HANDLE hProcess = nullptr;
	{
		std::wstring args = MakeParams(p);
		SHELLEXECUTEINFO sei = { sizeof(sei) };

		wchar_t thisModuleFileName[MAX_PATH];
		::GetModuleFileName(nullptr, thisModuleFileName, _countof(thisModuleFileName));

		sei.fMask = 0 | SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
		sei.hwnd = NULL;
		sei.lpVerb = L"runas";
		sei.lpFile = thisModuleFileName;
		sei.lpParameters = args.c_str();
		sei.lpDirectory = NULL;
		sei.nShow = SW_SHOWNORMAL;

		::ShellExecuteEx(&sei);

		if ((int)sei.hInstApp <= 32) {
			::CloseHandle(sei.hProcess);
			::CloseHandle(hPipe);
			::CloseHandle(hStderrPipe);
			return E_FAIL;
		}

		hProcess = sei.hProcess;
	}
	
	BOOL connected = ::ConnectNamedPipe(hPipe, NULL) ? TRUE : (::GetLastError() == ERROR_PIPE_CONNECTED);

	if (!connected) {
		DWORD lastError = ::GetLastError();

		::CloseHandle(hPipe);
		::CloseHandle(hStderrPipe);

		return lastError ? lastError : E_FAIL;
	}

	BOOL connectedStderr = ::ConnectNamedPipe(hStderrPipe, NULL) ? TRUE : (::GetLastError() == ERROR_PIPE_CONNECTED);
	if (!connectedStderr) {
		DWORD lastError = ::GetLastError();
		::CloseHandle(hStderrPipe);

		std::wcerr << L"Warning: failed to connect to stderr pipe: " << lastError << std::endl;
	}

	std::wcerr << L"Piping with elevated process id " << ::GetProcessId(hProcess) << L"!" << std::endl;

	if (!hOutput) {
		hOutput = hPipe;
	}
	else if (!hInput) {
		hInput = hPipe;
	}

	// run!
	{
		constexpr size_t buflen = 0x10000;
		std::unique_ptr<BYTE[]> buf(new BYTE[buflen]);

		__int64 totalBytes = 0;
		while (hInput && hOutput) {
			DWORD dwBytesRead = 0;
			if (hInput && !::ReadFile(hInput, buf.get(), buflen, &dwBytesRead, nullptr)) {
				DWORD err = ::GetLastError();
				hInput = nullptr;
				hOutput = nullptr;
			}
			DWORD dwBytesWritten = 0;
			if (dwBytesRead && hOutput && !::WriteFile(hOutput, buf.get(), dwBytesRead, &dwBytesWritten, nullptr)) {
				DWORD err = ::GetLastError();
				hInput = nullptr;
				hOutput = nullptr;
			}
			totalBytes += dwBytesWritten;

			if (hStderrPipe) {
				DWORD dwBytesRead = 0;
				DWORD dwBytesAvail = 0;
				DWORD dwBytesInMessage = 0;
				do {
					dwBytesRead = 0;
					dwBytesAvail = 0;
					dwBytesInMessage = 0;

					if (!::PeekNamedPipe(hStderrPipe, nullptr, 0, &dwBytesRead, &dwBytesAvail, &dwBytesInMessage)) {
						break;
					}

					if (0 == dwBytesAvail) {
						break;
					}

					dwBytesRead = 0;
					if (!::ReadFile(hStderrPipe, buf.get(), min(dwBytesAvail, buflen - 1), &dwBytesRead, nullptr)) {
						DWORD err = ::GetLastError();
						::CloseHandle(hStderrPipe);
						hStderrPipe = nullptr;
						break;
					}

					if (dwBytesRead > 0) {
						dwBytesAvail -= dwBytesRead;

						*(buf.get() + dwBytesRead) = 0;

						auto szErr = (const char*)buf.get();

						std::wcerr << szErr << std::flush;
					}
				} while (dwBytesAvail > 0);
			}
		}
	}

	::CloseHandle(hPipe);
	::CloseHandle(hStderrPipe);

	::WaitForSingleObject(hProcess, 5000);

	DWORD dwExitCode = 0;
	::GetExitCodeProcess(hProcess, &dwExitCode);

	::CloseHandle(hProcess);

	return dwExitCode;
}

HRESULT Run(params p)
{
	HANDLE hFile = nullptr;

	HANDLE hStdIn = ::GetStdHandle(STD_INPUT_HANDLE);
	HANDLE hStdOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
	HANDLE hStdErr = ::GetStdHandle(STD_ERROR_HANDLE);

	if (0 == p.to.find(LR"(\\.\pipe\mssqlPipe_)")) {
		::WaitNamedPipe(p.to.c_str(), NMPWAIT_USE_DEFAULT_WAIT);
	}
	if (0 == p.from.find(LR"(\\.\pipe\mssqlPipe_)")) {
		::WaitNamedPipe(p.from.c_str(), NMPWAIT_USE_DEFAULT_WAIT);
	}

	if (p.isBackup()) {
		if (p.to.empty()) {
			hFile = hStdOut;
		}
		else {
			hFile = ::CreateFile(p.to.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (!hFile || INVALID_HANDLE_VALUE == hFile) {
				DWORD ret = ::GetLastError();
				std::wcerr << ret << L": Failed to open " << p.to << std::endl;
				return E_FAIL;
			}
		}
	}
	else if (p.isRestore()) {
		if (!p.to.empty() && INVALID_FILE_ATTRIBUTES == ::GetFileAttributes(p.to.c_str())) {
			int ret = ::SHCreateDirectoryEx(nullptr, p.to.c_str(), nullptr);
			if (ret && ret != ERROR_FILE_EXISTS && ret != ERROR_ALREADY_EXISTS) {
				std::wcerr << ret << L": Failed to create restore to path. Continuing (SQL Server may have access)... " << p.from << std::endl;
			}
		}
		if (p.from.empty()) {
			hFile = hStdIn;
		}
		else {
			hFile = ::CreateFile(p.from.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (!hFile || INVALID_HANDLE_VALUE == hFile) {
				DWORD ret = ::GetLastError();
				std::wcerr << ret << L": Failed to open " << p.from << std::endl;
				return E_FAIL;
			}
		}
	}
	else if (p.isPipe()) {
		if (iequals(p.subcommand, L"to")) {
			if (p.from.empty()) {
				hFile = hStdIn;
			}
			else {
				hFile = ::CreateFile(p.from.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (!hFile || INVALID_HANDLE_VALUE == hFile) {
					DWORD ret = ::GetLastError();
					std::wcerr << ret << L": Failed to open " << p.from << std::endl;
					return E_FAIL;
				}
			}
		}
		else if (iequals(p.subcommand, L"from")) {
			if (p.to.empty()) {
				hFile = hStdOut;
			}
			else {
				hFile = ::CreateFile(p.to.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (!hFile || INVALID_HANDLE_VALUE == hFile) {
					DWORD ret = ::GetLastError();
					std::wcerr << ret << L": Failed to open " << p.to << std::endl;
					return E_FAIL;
				}
			}
		}
	}

	if (!hFile) {
		std::wcerr << L"missing file" << std::endl;
		return E_FAIL;
	}

	HRESULT hr = S_OK;
	
	VirtualDevice vd(p.instance, p.device);
	hr = vd.Create();
	if (!SUCCEEDED(hr)) {
		if (E_ACCESSDENIED == hr && !p.flags.noelevate) {
			std::wcerr << L"Run failed with E_ACCESSDENIED!" << std::endl;
			std::wcerr << L"Attempting to elevate and redirect io..." << std::endl;

			std::wstring unique = make_guid().substr(1, 8);

			std::wstring namedPipe;
			{
				std::wostringstream o;
				o << LR"(\\.\pipe\mssqlPipe_)" << L"stdio_" << std::setfill(L'0') << std::setw(8) << std::hex << ::GetCurrentProcessId() << std::dec << L"_" << unique;
				namedPipe = o.str();
			}

			std::wstring stderrPipe;
			{
				std::wostringstream o;
				o << LR"(\\.\pipe\mssqlPipe_)" << L"stderr_" << std::setfill(L'0') << std::setw(8) << std::hex << ::GetCurrentProcessId() << std::dec << L"_" << unique;
				stderrPipe = o.str();
			}

			HANDLE hInput = nullptr;
			HANDLE hOutput = nullptr;

			p.flags.noelevate = true;
			p.flags.tee = stderrPipe;
			
			if (p.isBackup()) {
				hOutput = hFile;
				p.to = namedPipe;
			}
			else if (p.isRestore()) {
				hInput = hFile;
				p.from = namedPipe;
			}
			else if (p.isPipe()) {
				if (iequals(p.subcommand, L"to")) {
					hInput = hFile;
					p.from = namedPipe;
				} else if (iequals(p.subcommand, L"from")) {
					hOutput = hFile;
					p.to = namedPipe;
				}
			}

			hr = Elevate(p, hInput, hOutput, namedPipe, stderrPipe);
			if (!SUCCEEDED(hr)) {
				// oh no
			}
		}
	} else if (p.isBackup()) {
		hr = RunBackup(vd, p, hFile);
	}
	else if (p.isRestore()) {
		hr = RunRestore(vd, p, hFile);
	}
	else if (p.isPipe()) {
		hr = RunPipe(vd, p, hFile);
	}
	else {		
		std::wcerr << L"unexpected command " << p.command << std::endl;
		hr = E_FAIL;
	}
	
	if (hFile != hStdOut && hFile != hStdIn && hFile != hStdErr) {
		::CloseHandle(hFile);
		hFile = nullptr;
	}

	return hr;
}


/****/


int wmain(int argc, wchar_t* argv[])
{
	CoInit comInit;

#ifdef _DEBUG
	bool waitForDebugger = true;
	waitForDebugger = false;
	if (waitForDebugger && !::IsDebuggerPresent()) {
		std::wcerr << L"Waiting for debugger..." << std::endl;
		::Sleep(10000);
	}
#endif

	struct TeeAndRedirect_wcerr
	{
		TeeAndRedirect_wcerr(const wchar_t* fileName)
			: out(fileName, std::ios::out)
			, wcerrbuf(std::wcerr.rdbuf())
			, tee(wcerrbuf, out.rdbuf())
		{
			std::wcerr.rdbuf(&tee);
		}

		~TeeAndRedirect_wcerr()
		{
			std::wcerr.rdbuf(wcerrbuf);
		}

		std::wofstream out;
		std::basic_streambuf<wchar_t>* wcerrbuf = nullptr;
		wteebuf tee;
	};
		
	std::wcerr << L"\nmssqlPipe " << mssqlPipe_Version << L"\n" << std::endl;

	params p = ParseParams(argc, argv, false);

	std::unique_ptr<TeeAndRedirect_wcerr> pTeeAndRedirectWcerr;

	if (!p.flags.tee.empty()) {
		if (0 == p.flags.tee.find(LR"(\\.\pipe\mssqlPipe_)")) {
			::WaitNamedPipe(p.flags.tee.c_str(), NMPWAIT_USE_DEFAULT_WAIT);
		}
		pTeeAndRedirectWcerr = std::make_unique<TeeAndRedirect_wcerr>(p.flags.tee.c_str());

		std::wcerr << L"tee stderr to " << p.flags.tee << std::endl;
	}

	if (p.flags.test) {
#ifdef _DEBUG
		bool parseParamsResult = TestParseParams();
		assert(parseParamsResult);
#endif
	}
	
	if (SUCCEEDED(p.hr) && !p.command.empty()) {
		p.hr = Run(p);
	}

	if (E_ACCESSDENIED == p.hr) {
		std::wcerr << L"Run failed with E_ACCESSDENIED; mssqlPipe may need to run as an administrator." << std::endl;
	}

	pTeeAndRedirectWcerr.reset();

#ifdef _DEBUG
	::Sleep(5000);
#endif

	return p.hr;
}

