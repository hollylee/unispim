/**	杂项工具函数库。包括：
 *		1. 共享内存的分配与释放。
 *		2. 系统时间函数
 */
#include <shlobj.h>
#include <windows.h>
#include <kernel.h>
#include <time.h>
#include <io.h>
#include <fcntl.h>
#include <zlib/zlib.h>
#include <sys/stat.h>
#include <tchar.h>
#include <Sddl.h>
#include <Aclapi.h>

//此宏定义的原因请参见头文件中的说明。
#define	_IN_LOG_C_FILE
#include <utility.h>
#undef	_IN_LOG_C_FILE

static SHAREDMEMORYINFO shared_info[MAX_SHARED_MEMORY_COUNT] = { 0, };

/*	返回系统的Tick数（从系统启动开始的时钟步数）
 *	参数：无
 *	返回：
 *		当前系统的TickCount
 */
int GetCurrentTicks()
{
	return (int)GetTickCount();
}

/*	带有长度的复制字符串（为了进行移植的需要）。
 *	参数：
 *		target				目标地址
 *		source				源地址
 *	返回：无
*/
void CopyPartString(TCHAR *target, const TCHAR *source, int length)
{
	int i;

	for (i = 0; i < length && source[i]; i++)
		target[i] = source[i];

	target[i] = 0;
}

/*	输出汉字。
 *	参数：
 *		hz			汉字
 *	返回：无
 */
void OutputHz(HZ hz)
{
	printf("%c%c", hz % 0x100, hz / 0x100);
}

//默认LOG文件名称
static const TCHAR *default_log_file_name = L"c:\\pim.log";
static int last_time_tick = 0;
static FILE *log_fd;

/*	在LOG文件中写出信息。
 *	当LOG文件没有被初始化的时候，log信息将丢失。
 *	参数：
 *		id				一般为文件名字与函数名字的组合
 *		format			输出格式，与printf相同
 *		...				附带的可变参数
 *	返回：
 *		无
 */
void Log(const TCHAR *id, const TCHAR *format, ...)
{
	//FILE *log_fd;
	time_t cur_time;
	struct tm *time_info;
	TCHAR tmp[0x20];
	va_list marker;
	int passed_time;
	int need_rom;

#ifndef	_DEBUG
	return;
#endif

	__try
	{
		if(!log_fd)
			return;
		passed_time = GetCurrentTicks() - last_time_tick;
		if (!last_time_tick)
			passed_time = 0;

		last_time_tick += passed_time;
		need_rom = !FileExists(default_log_file_name);

		if (need_rom)
			_ftprintf(log_fd, L"%c", 0xFEFF);

		va_start(marker, format);
		cur_time = time(0);
		time_info = localtime(&cur_time);
		_tcsftime(tmp, _SizeOf(tmp), L"%H:%M:%S", time_info);
		_ftprintf(log_fd, L"%s(%06d) %-24s(%x)\t", tmp, passed_time, id, GetCurrentThreadId());

		//输出LOG信息
		_vftprintf(log_fd, format, marker);

		//如果输入信息最后没有回车，则加上回车
		if (_tcslen(format) && format[_tcslen(format) - 1] != '\n')
			_ftprintf(log_fd, L"\n");

	}
	__except(1)
	{
	}
}

/*	初始化LOG模块。打开LOG文件，或者将LOG文件置空，重新开始。
 *	参数：
 *		log_file_name			LOG文件名称，如果为空，则为默认的LOG文件名称
 *		restart					是否重新开始LOG文件，如果为1，则重新创建LOG文件；
 *								如果为0，则在原有的文件上追加。
 *	返回值：
 *		成功：1，失败：0
 */
int LogInit(int restart)
{
#ifndef	_DEBUG
	return 1;
#endif

	last_time_tick = GetCurrentTicks();

	if (restart)
		_tunlink(default_log_file_name);

	if(!log_fd)//在初始化的时候，open日志文件
	{
		log_fd = _tfopen(default_log_file_name, L"a");
		if (!log_fd)
			return 0;
		_setmode(_fileno(log_fd), _O_U16TEXT);
	}
	Log(LOG_ID, L"日志开始记录");

	return 1;
}

int FreeLog()
{
#ifndef	_DEBUG
	return 1;
#endif
	if(log_fd)
		fclose(log_fd);
	return 1;
}

/*	将文件装载到缓冲区。
 *	请注意：缓冲区的大小必须满足文件读取的要求，不能小于文件的长度。
 *	参数：
 *		file_name			文件全路径名称
 *		buffer				缓冲区
 *		buffer_length		缓冲区长度
 *	返回：
 *		成功：返回读取的长度，失败：-1
 */
int LoadFromFile(const TCHAR *file_name, void *buffer, int buffer_length)
{
	FILE *fd;
	int length;

	Log(LOG_ID, L"读取文件%s到内存%p", file_name, buffer);
	fd = _tfopen(file_name, TEXT("rb"));
	if (!fd)
	{
		Log(LOG_ID, L"文件打开失败");
		return 0;
	}

	length = (int)fread(buffer, 1, buffer_length, fd);
	fclose(fd);

	if (length < 0)
	{
		Log(LOG_ID, L"文件读取失败");
		return 0;
	}

	Log(LOG_ID, L"文件读取成功, 长度:%d", length);
	return length;
}

/*	将内存保存到文件。如果目标存在，则覆盖。
 *	参数：
 *		file_name			文件全路径名称
 *		buffer				缓冲区指针
 *		buffer_length		文件长度
 *	返回：
 *		成功：1，失败：0
 */
int SaveToFile(const TCHAR *file_name, void *buffer, int buffer_length)
{
	FILE *fd;
	int length;

	Log(LOG_ID, L"保存内存%p到文件%s，长度：%d", buffer, file_name, buffer_length);

	fd = _tfopen(file_name, TEXT("wb"));
	if (!fd)
	{
		TCHAR dir_name[MAX_PATH];
		int  i, index, ret;

		Log(LOG_ID, L"文件打开失败");

		//可能需要创建目录
		//1. 寻找当前文件的目录
		index = 0;
		_tcscpy(dir_name, file_name);
		for (i = 0; dir_name[i]; i++)
			if (dir_name[i] == '\\')
				index = i;

		if (!index)
			return 0;

		dir_name[index] = 0;		//dir_name中包含有目录名字
		ret = SHCreateDirectoryEx(0, dir_name, 0);
		if (ret != ERROR_SUCCESS)
			return 0;

		//创建目录成功，再次打开
		fd = _tfopen(file_name, TEXT("wb"));
		if (!fd)
			return 0;
	}

	length = (int)fwrite(buffer, 1, buffer_length, fd);
	fclose(fd);

	if (length != buffer_length)
	{
		Log(LOG_ID, L"文件写入失败");
		return 0;
	}

	Log(LOG_ID, L"文件写入成功");
	return length;
}

/*	获得文件长度。
 *	参数：
 *		file_name			文件名称
 *	返回：
 *		文件长度，-1标识出错。
 */
int GetFileLength(const TCHAR *file_name)
{
	struct _stat f_data;

	if (_tstat(file_name, &f_data))
		return -1;

	return (int) f_data.st_size;
}

/*	存储共享内存句柄。
 *	参数：
 *		handle			共享内存句柄
 *		pointer			指针
 *	返回：无
 */
static void StoreSharedMemoryHandle(HANDLE handle, void *pointer)
{
	int i;

	//找到一个空位
	for (i = 0; i < MAX_SHARED_MEMORY_COUNT; i++)
		if (shared_info[i].pointer == 0)
		{
			shared_info[i].pointer = pointer;
			shared_info[i].handle = handle;

			return;
		}
}

/*	获得共享内存区域指针。
 *	参数：
 *		shared_name			共享内存名称
 *	返回：
 *		没有找到：0
 *		找到：指向内存的指针
 */
void *GetSharedMemory(const TCHAR *shared_name)
{
	HANDLE handle;
	char *p;

	//在IE7+Vista环境中，如果启用了保护模式，则IE7不能对词库进行写操作，加入
	//写操作要求后，将会使共享失败。
	//共享内存的写入，只包括用户词库的写入，所以要对用户词库更新进行判断

	//获得共享句柄
	handle = OpenFileMapping(
		FILE_MAP_READ | FILE_MAP_WRITE,	//需要读写权限
		0,								//不需要继承
		shared_name);				    //共享名字

	if (!handle)
	{
		Log(LOG_ID, L"打开共享失败，共享名称:%s", shared_name);
		return 0;
	}

	//映射
	p = (char*)MapViewOfFile(
		handle,							//共享内存句柄
		FILE_MAP_READ | FILE_MAP_WRITE,	//需要读写权限
		0,
		0,						//偏移从0开始
		0);						//最大长度

	if (!p)
		Log(LOG_ID, L"文件映射失败，共享名称:%s，句柄:%d", shared_name, handle);
	else
	{
		Log(LOG_ID, L"打开共享成功，共享名称:%s", shared_name);
		StoreSharedMemoryHandle(handle, p);
	}

	return p;
}

/*	获得只读的共享内存区域指针（针对IE7）
 *	参数：
 *		shared_name			共享内存名称
 *	返回：
 *		没有找到：0
 *		找到：指向内存的指针
 */
void *GetReadOnlySharedMemory(const TCHAR *shared_name)
{
	HANDLE handle;
	char *p;

	//在IE7+Vista环境中，如果启用了保护模式，则IE7不能对词库进行写操作，加入
	//写操作要求后，将会使共享失败。
	//共享内存的写入，只包括用户词库的写入，所以要对用户词库更新进行判断

	//获得共享句柄
	handle = OpenFileMapping(
		FILE_MAP_READ,					//需要读权限
		0,								//不需要继承
		shared_name);				    //共享名字

	if (!handle)
	{
		Log(LOG_ID, L"打开共享失败，共享名称:%s", shared_name);
		return 0;
	}

	//映射
	p = (char*)MapViewOfFile(
		handle,							//共享内存句柄
		FILE_MAP_READ,					//需要读权限
		0,
		0,						//偏移从0开始
		0);						//最大长度

	if (!p)
		Log(LOG_ID, L"文件映射失败，共享名称:%s，句柄:%d", shared_name, handle);
	else
	{
		Log(LOG_ID, L"打开共享成功，共享名称:%s", shared_name);
		StoreSharedMemoryHandle(handle, p);
	}

	return p;
}

BOOL SetObjectToLowIntegrity(HANDLE hObj)
{
	#define LABEL_SECURITY_INFORMATION (0x00000010L)
    const wchar_t* LOW_INTEGRITY_SDDL_SACL_W = L"S:(ML;;NW;;;LW)";

    BOOL rst = FALSE;
    DWORD dwErr = ERROR_SUCCESS;
    PSECURITY_DESCRIPTOR pSD = NULL;
    PACL pSacl = NULL;
    BOOL fSaclPresent = FALSE;
    BOOL fSaclDefaulted = FALSE;

    if (ConvertStringSecurityDescriptorToSecurityDescriptor(LOW_INTEGRITY_SDDL_SACL_W, SDDL_REVISION_1, &pSD, NULL))
    {
        if (GetSecurityDescriptorSacl(pSD, &fSaclPresent, &pSacl, &fSaclDefaulted))
        {
            dwErr = SetSecurityInfo(hObj, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION, NULL, NULL, NULL, pSacl);
            rst	  = (ERROR_SUCCESS == dwErr);
        }

        LocalFree(pSD);
    }

    return rst;
}

/*	创建共享内存区
 *	参数：
 *		shared_name			共享内存名称
 *		length				共享内存长度
 *	返回：
 *		创建失败：0
 *		成功：指向内存的指针
 */
void *AllocateSharedMemory(const TCHAR *shared_name, int length)
{
	HANDLE handle;
	SECURITY_DESCRIPTOR	sd;
	SECURITY_ATTRIBUTES	sa;
	char *p;

	InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
	SetSecurityDescriptorDacl(&sd, 1, 0, 1);
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = &sd;
	sa.bInheritHandle = 0;

	//创建共享
	handle = CreateFileMapping(
		INVALID_HANDLE_VALUE,			//不直接使用文件共享
		&sa,							//默认的安全描述符
		PAGE_READWRITE,					//页面可以写
		0,								//长度高32位
		length,							//长度低32位
		shared_name);				    //共享名字

	if (!handle)
	{
		Log(LOG_ID, L"创建共享失败，共享名称:%s，长度:%d, err=%d", shared_name, length, GetLastError());
		return 0;
	}

	SetObjectToLowIntegrity(handle);

	//映射
	p = (char*)MapViewOfFile(
		handle,							//共享内存句柄
		FILE_MAP_READ | FILE_MAP_WRITE,	//需要读写权限
		0,
		0,								//偏移从0开始
		length);						//最大长度

	if (!p)
	{
		int err = GetLastError();
		Log(LOG_ID, L"文件映射失败，共享名称:%s，句柄:%d, 长度:%d, Err:%d", shared_name, handle, length, err);
	}
	else
	{
		StoreSharedMemoryHandle(handle, p);
		Log(LOG_ID, L"分配共享内存成功，长度:%d", length);
	}

	return p;
}

/*	释放共享内存区
 *	参数：
 *		shared_name			共享内存名称
 *		pointer				共享内存指针
 *	返回：无
 */
void FreeSharedMemory(const TCHAR *shared_name, char *pointer)
{
	int i;

	//解除映射
	UnmapViewOfFile(pointer);

	//获得共享句柄
	for (i = 0; i < MAX_SHARED_MEMORY_COUNT; i++)
		if (shared_info[i].pointer == pointer)
		{
			CloseHandle(shared_info[i].handle);
			shared_info[i].handle  = 0;
			shared_info[i].pointer = 0;

			return;
		}
}

/*	窗口移动函数。
 *	注意：同一时刻，只能有一个窗口移动。
 */
int   drag_mode;
static POINT cursor_origin_pos;
static POINT window_origin_pos;

RECT GetMonitorRectFromPoint(POINT point)
{
	HMONITOR  monitor;
	MONITORINFO monitor_info;
	RECT rect;

	SystemParametersInfo(SPI_GETWORKAREA, 0, &rect, 0);
	monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
	if (monitor)
	{
		monitor_info.cbSize = sizeof(MONITORINFO);
		monitor_info.rcWork = rect;
		GetMonitorInfo(monitor, &monitor_info);
		return monitor_info.rcWork;
	}

	return rect;
}

void MakeRectInRect(RECT *in_rect, RECT out_rect)
{
	int width = in_rect->right - in_rect->left;
	int height = in_rect->bottom - in_rect->top;

	if (in_rect->right > out_rect.right)
		in_rect->left = out_rect.right - width;
	if (in_rect->left < out_rect.left)
		in_rect->left = out_rect.left;

	if (in_rect->bottom > out_rect.bottom)
		in_rect->top = out_rect.bottom - height;
	if (in_rect->top < out_rect.top)
		in_rect->top = out_rect.top;

	in_rect->right = in_rect->left + width;
	in_rect->bottom = in_rect->top + height;
}

void DragStart(HWND window)
{
	if (drag_mode)
		return;

	drag_mode = 1;
	SetCapture(window);
	GetCursorPos(&cursor_origin_pos);
}

void DragMove(HWND window)
{
	RECT  win_rect;
	POINT new_cursor_pos;
	int new_x, new_y;

	if (!drag_mode)
		return;

	GetCursorPos(&new_cursor_pos);
	GetWindowRect(window, &win_rect);

	new_x = win_rect.left + new_cursor_pos.x - cursor_origin_pos.x;
	new_y = win_rect.top + new_cursor_pos.y - cursor_origin_pos.y;
	cursor_origin_pos = new_cursor_pos;

	MoveWindow(window, new_x, new_y, win_rect.right - win_rect.left, win_rect.bottom - win_rect.top, 1);
}

void DragEnd(HWND window)
{
	POINT point;
	RECT  monitor_rect;
	RECT  window_rect;

	if (!drag_mode)
		return;

	drag_mode = 0;
	ReleaseCapture();

	GetCursorPos(&point);
	monitor_rect = GetMonitorRectFromPoint(point);
	GetWindowRect(window, &window_rect);

	MakeRectInRect(&window_rect, monitor_rect);
	MoveWindow(window, window_rect.left, window_rect.top, window_rect.right - window_rect.left, window_rect.bottom - window_rect.top, 1);
}

//需要转换的字符
static const UINT shift_chars[] =
{
	0xc0, 0x7ec0, 1,
	0x31, 0x2131, 1,
	0x32, 0x4032, 1,
	0x33, 0x2333, 1,
	0x34, 0x2434, 1,
	0x35, 0x2535, 1,
	0x36, 0x5e36, 1,
	0x37, 0x2637, 1,
	0x38, 0x2a38, 1,
	0x39, 0x2839, 1,
	0x30, 0x2930, 1,
	0xbd, 0x5fbd, 1,
	0xbb, 0x2bbb, 1,
	0xdc, 0x7cdc, 1,
	0xdb, 0x7bdb, 1,
	0xdd, 0x7ddd, 1,
	0xba, 0x3aba, 1,
	0xde, 0x22de, 1,
	0xbc, 0x3cbc, 1,
	0xbe, 0x3ebe, 1,
	0xbf, 0x3fbf, 1,
	0xc0, 0x60c0, 0,
	0xbd, 0x2dbd, 0,
	0xbb, 0x3dbb, 0,
	0xdc, 0x5cdc, 0,
	0xdb, 0x5bdb, 0,
	0xdd, 0x5ddd, 0,
	0xba, 0x3bba, 0,
	0xde, 0x27de, 0,
	0xbc, 0x2cbc, 0,
	0xbe, 0x2ebe, 0,
	0xbf, 0x2fbf, 0
};

UINT CompleteVirtualKey(UINT virtual_key, UINT scan_code, CONST LPBYTE key_state)
{
	int i = 0;
	UINT ret = virtual_key;

	if ((virtual_key > 0xFF) || ((virtual_key >= VK_PRIOR) && (virtual_key <= VK_HELP)))
		return ret;

	if (isupper(virtual_key))
	{
		/*大写键按下，并且小写键按下 or 大写键未按下，并且小写键也未按下*/
		if (((key_state[VK_CAPITAL] & 0x80) && (key_state[VK_SHIFT] & 0x80)) ||
			(!(key_state[VK_CAPITAL] & 0x80) && !(key_state[VK_SHIFT] & 0x80)))
			ret = ((virtual_key - 'A' + 'a') << 8) + virtual_key;
	}
	else
	{
		for (i = 0; i < sizeof(shift_chars) / sizeof(shift_chars[0]); i += 3)
		{
			if (virtual_key == shift_chars[i])
			{
				if (((key_state[VK_SHIFT] & 0x80) && shift_chars[i + 2]) ||
					((!(key_state[VK_SHIFT] & 0x80)) && (!shift_chars[i + 2])))
				{
					ret = shift_chars[i + 1];
					break;
				}
			}
		}
	}

	//not found
	if (ret == virtual_key)
	{
		//小键盘上的数字
		if (virtual_key >= VK_NUMPAD0 && virtual_key <= VK_NUMPAD9)
			ret = ((virtual_key - 0x30) << 8) + virtual_key;
		else
			ret = (virtual_key << 8) + virtual_key;
	}

	return ret;
}

/*	将VK解析成为可以判断的元素
 */
void TranslateKey(UINT virtual_key, UINT scan_code, CONST LPBYTE key_state, int *key_flag, TCHAR *ch, int no_virtual_key)
{
	int flag = 0;

	//SHIFT
	if (key_state[VK_SHIFT] & 0x80)
		flag |= KEY_SHIFT;
	if (scan_code == 0xc02a)
		flag |= KEY_LSHIFT;
	if (scan_code == 0xc036)
		flag |= KEY_RSHIFT;

	//CONTROL
	if (key_state[VK_CONTROL] & 0x80)
		flag |= KEY_CONTROL;
	if (scan_code == 0xc11d)
		flag |= KEY_RCONTROL;
	if (scan_code == 0xc01d)
		flag |= KEY_LCONTROL;

	//ALT
	if (key_state[VK_MENU] & 0x80)
		flag |= KEY_ALT;
	if (key_state[VK_LMENU] & 0x80)
		flag |= KEY_LALT;
	if (key_state[VK_RMENU] & 0x80)
		flag |= KEY_RALT;

	//CAPITAL
	if (key_state[VK_CAPITAL])
		flag |= KEY_CAPITAL;

	//部分软件传入的virtualkey没有前半部分的字符信息，只能通过其他方式来获取，例如vista下的onenote2003
	if ((virtual_key <= 0xFF) && no_virtual_key)
		virtual_key = CompleteVirtualKey(virtual_key, scan_code, key_state);

	//转换到ASC字符
	*ch = (virtual_key & 0xff0000) >> 16;

	*key_flag = flag;
}

/*	判断文件是否存在。
 */
int FileExists(const TCHAR *file_name)
{
	if (INVALID_FILE_ATTRIBUTES == GetFileAttributes(file_name))
		return 0;

	return 1;
}

/*	获得所有用户的Application目录
 *	一般为: c:\documents and setting\alluser\applicate data
 *	Vista为: c:\docs\alluser\app
 */
TCHAR *GetAllUserAppDirectory(TCHAR *dir)
{
	if (SHGetFolderPath(0, CSIDL_COMMON_APPDATA, 0, 0, dir))
	{
		Log(LOG_ID, L"SHGetFolderPath调用失败, err=%d", GetLastError());
		dir[0] = 0;
		return 0;
	}

	return dir;
}

/*	获得当前用户的Application目录
 *	一般为: c:\documents and setting\{username}\applicate data
 *	Vista为: c:\docs\{username}\app
 */
TCHAR *GetUserAppDirectory(TCHAR *dir)
{
	//vista 下放到appdata/locallow
	if (GetWindowVersion() >= 0x0600)
	{
		TCHAR lowdir[MAX_PATH];

		if (SHGetFolderPath(0, CSIDL_LOCAL_APPDATA, 0, 0, dir))
		{
			Log(LOG_ID, L"SHGetFolderPath 调用失败, err=%d", GetLastError());
			dir[0] = 0;
			return 0;
		}

		_tcscpy_s(lowdir, MAX_PATH, dir);
		_tcscat_s(lowdir, MAX_PATH, TEXT("Low"));

		if (FileExists(lowdir))
			_tcscat_s(dir, MAX_PATH, TEXT("Low"));
	}
	else if (SHGetFolderPath(0, CSIDL_APPDATA, 0, 0, dir))
	{
		Log(LOG_ID, L"SHGetFolderPath调用失败, err=%d", GetLastError());
		dir[0] = 0;
		return 0;
	}

	return dir;
}

/*	获得当前program files目录
 *	一般为: c:\program files
 */
TCHAR *GetProgramDirectory(TCHAR *dir)
{
	if (SHGetFolderPath(0, CSIDL_PROGRAM_FILES, 0, 0, dir))
	{
		Log(LOG_ID, L"SHGetFolderPath调用失败, err=%d", GetLastError());
		dir[0] = 0;
		return 0;
	}

	return dir;
}

/*	组合目录与文件，形成新的完整文件
 *	参数：
 *		dir			目录名
 *		file		文件名
 *		result		结果
 *	返回：
 */
TCHAR *CombineDirAndFile(const TCHAR *dir, const TCHAR *file, TCHAR *result)
{
	if (result != dir)
		_tcscpy(result, dir);

	if (dir[_tcslen(dir) - 1] != '\\')
		_tcscat(result, TEXT("\\"));

	if (file[0] == '\\')
		file++;

	_tcscat(result, file);

	return result;
}

/*	获得文件的绝对路径。
 *	存储于Config中的文件名字，可以为相对路径名字。
 *	如："theme/abc.jpg"，在/usr/app/unispim6目录中无法找到
 *	的话，则在/alluser/app/unispim6/中寻找，如果还找不到，则返回0
 *	如果文件为绝对路径，则找到直接返回。
 *	参数：
 *		file_name
 *		result;
 *	返回：
 *		0：没有找到文件（/usr、/allusr下皆没有）
 *		其他：新文件名（即原来的result）
 */
TCHAR *GetFileFullName(int type, const TCHAR *file_name, TCHAR *result)
{
	TCHAR dir[MAX_PATH];

	switch(type)
	{
	case TYPE_USERAPP:
		if (!GetUserAppDirectory(dir))
			return 0;
		break;

	case TYPE_ALLAPP:
		if (!GetAllUserAppDirectory(dir))
			return 0;
		break;

	case TYPE_PROGRAM:
		if (!GetProgramDirectory(dir))
			return 0;
		break;

	case TYPE_TEMP:
		if (!GetTempPath(_SizeOf(dir), dir))
			return 0;
		break;

	default:
		return 0;
	}

	CombineDirAndFile(dir, file_name, result);
	return result;
}

/*	Ansi字符串转换到UTF16
 */
void AnsiToUtf16(const char *name, wchar_t *wname, int nSize)
{
	MultiByteToWideChar(936, 0, name, (int)strlen(name) + 1, wname, nSize);
}

void Utf16ToAnsi(const wchar_t *wchars, char *chars, int nSize)
{
	WideCharToMultiByte(936, 0, wchars, -1, chars, nSize, NULL, FALSE);
}

void UCS32ToUCS16(const UC UC32Char, TCHAR *buffer)
{
	buffer[1] = 0;

	if (UC32Char > 0x10FFFF || (UC32Char >= 0xD800 && UC32Char <= 0xDFFF))
	{
		buffer[0] = '?';
		return;
	}

	if (UC32Char < 0x10000)
		buffer[0] = (TCHAR)UC32Char;
	else
	{
		buffer[0] = (UC32Char - 0x10000) / 0x400 + 0xD800;
		buffer[1] = (UC32Char - 0x10000) % 0x400 + 0xDC00;
		buffer[2] = 0;
	}
}

//判断一个4字节TChar数组，是由几个汉字组成的。返回值：0，1，2
int UCS16Len(TCHAR *buffer)
{
	size_t L = _tcslen( buffer );
	if ( L == 0 )
		return 0;
	if ( L > 2 )
	  return 2;
	if ( L == 2 )
	{
	  if (( buffer[0] >= 0xD800 ) && ( buffer[0] <= 0xDFFF ))
		return 1;
	  else
		return 2;
	}
	return 1;
}

//从ucs16转为ucs32，只转一个汉字，多于一个汉字，返回0。
UC UCS16ToUCS32(TCHAR *buffer)
{
	size_t L;
	int L1;
	L =  _tcslen( buffer );
	L1 = UCS16Len( buffer ); 

	if ( L1 != 1 )
		return 0;
	if ( L == 1 )
		return (int)buffer[0];
	else
		return (( buffer[0] - 0xD800 ) << 10) + ( buffer[1] - 0xDC00 ) + 0x10000;
}

/**	获得一行文本，并且除掉尾回车
 */
int GetLineFromFile(FILE *fr, TCHAR *line, int length)
{
	TCHAR *p = line, ch;

	ch = _fgettc(fr);
	if (WEOF == ch)
		return 0;

	while (length-- && ch != WEOF)
	{
		*p = ch;
		if (0xa == ch)
			break;

		ch = _fgettc(fr);

		if (0xd == *p && 0xa == ch)
			break;

		p++;
	}

	*p = 0;

	return 1;
}

/**	从文件中读出一个字符串
 *	参数：
 *		file		文件指针
 *		string		读入的字符串缓冲区
 *		length		缓冲区长度
 *	返回：
 *		字符串的长度
 */
int GetStringFromFile(FILE *file, TCHAR *string, int length)
{
	TCHAR ch = 0x09;
	int count = 0;

	*string = 0;

	/* 过滤分隔符 */
	while(0x09 == ch || 0xd == ch || 0xa == ch)
		ch = _fgettc(file);

	if (WEOF == ch)
		return 0;

	while (length-- && ch != WEOF && 0x09 != ch && 0xd != ch && 0xa != ch)
	{
		*string++ = ch;
		count++;

		ch = _fgettc(file);
	}

	*string = 0;

	return count;
}

/**	删除字符串中的首尾空白字符
 */
void TrimString(TCHAR *line)
{
	TCHAR *p;
	TCHAR *start, *end;
	int  i;

	if (!*line)
		return;

	p = line;

	//除掉首部空白
	while(*p)
	{
		if (*p != 0xd && *p != 0xa && *p != ' ' && *p != 0x9)
			break;

		p++;
	}

	start = end = p;

	while(*p)
	{
		if (*p != 0xd && *p != 0xa && *p != ' ' && *p != 0x9)
			end = p;

		p++;
	}

	for (i = 0; i <= end - start; i++)
		line[i] = start[i];

	line[i] = 0;
}

/**	获得当前的时间数据
 */
void GetTimeValue(int *year, int *month, int *day, int *hour, int *minute, int *second, int *msecond)
{
	SYSTEMTIME system_time;
	GetLocalTime(&system_time);

	*year    = system_time.wYear;
	*month   = system_time.wMonth;
	*day     = system_time.wDay;
	*hour    = system_time.wHour;
	*minute  = system_time.wMinute;
	*second  = system_time.wSecond;
	*msecond = system_time.wMilliseconds;
}

/**	使字符串适合于缓冲区的大小，如果过大则截取在汉字的边界上
 */
void MakeStringFitLength(TCHAR *string, int length)
{
	int index, last_index;

	index = last_index = 0;
	while(string[index] && index < length)
	{
		index++;

		if (index >= length)
			break;

		last_index = index;
	}

	string[last_index] = 0;
}

/**	执行程序
 */
void ExecuateProgram(const TCHAR *program_name, const TCHAR *args, const int is_url)
{
	STARTUPINFO	start_info;
	PROCESS_INFORMATION	process_info;
	TCHAR buffer[MAX_PATH];

	memset(&start_info, 0, sizeof(start_info));
	start_info.cb = sizeof(start_info);
	memset(&process_info, 0, sizeof(process_info));

	_tcscpy_s(buffer, _SizeOf(buffer), program_name);
	if (!_tcsstr(buffer, TEXT(".")) && !_tcsstr(buffer, TEXT("://")))
		_tcscat_s(buffer, _SizeOf(buffer), TEXT(".exe"));

	if (is_url && !args)
	{
		HKEY  reg_key;					//注册表KEY
		DWORD data_type;				//注册表项的类型
		DWORD data_length = MAX_PATH;	//配置内容长度
		TCHAR value[MAX_PATH];

		if (RegOpenKey(HKEY_CLASSES_ROOT, TEXT("HTTP\\shell\\open\\command"), &reg_key) == ERROR_SUCCESS)
		{
			RegQueryValueEx(reg_key, TEXT(""), 0, &data_type, (LPBYTE)value, &data_length);

			if (!_tcsicmp(value, TEXT(""))/* || !_tcsicmp(value, TEXT("open"))*/)
			{
				ShellExecute(0, TEXT("open"), TEXT("iexplore"), buffer, 0, SW_SHOWNORMAL);
				return;
			}
		}
	}

	ShellExecute(0, TEXT("open"), buffer, args, 0, SW_SHOWNORMAL);
}

void ExecuateProgramWithArgs(const TCHAR *cmd_line)
{
	const TCHAR *args;
	TCHAR exe[MAX_PATH];
	int  i, str_length;

	str_length = (int)_tcslen(cmd_line);
	for (i = 0; i < str_length; i++)
		if (cmd_line[i] == ' ')
			break;

	if (i == str_length)		//没有参数
	{
		ExecuateProgram(cmd_line, 0, 0);
		return;
	}

	args = cmd_line + i;
	_tcsncpy_s(exe, _SizeOf(exe), cmd_line, min(i, _SizeOf(exe) - 1));

	ExecuateProgram(exe, args, 0);
}

/**	系统消息窗口过程
 */
LRESULT WINAPI WaitingWindowProcedure(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	TCHAR szMsg[0x200];

	switch (uMsg)
	{
	case WM_CREATE:
		{
			HDC		hdc;
			SIZE	sizeText;
			RECT	rcWorkArea;

			GetWindowText(hwnd, szMsg, _SizeOf(szMsg) - 1);
			hdc = GetDC(hwnd);
			GetTextExtentPoint32(hdc, szMsg, (int)_tcslen(szMsg), &sizeText);
			ReleaseDC(hwnd, hdc);
			sizeText.cx += 60;
			sizeText.cy += 20;
			SystemParametersInfo(SPI_GETWORKAREA, 0, &rcWorkArea, FALSE);
			MoveWindow(hwnd, (rcWorkArea.right - sizeText.cx) / 2, (rcWorkArea.bottom - sizeText.cy) / 2,
				sizeText.cx, sizeText.cy, TRUE);

			return	0;
		}

	case WM_PAINT:
		{
			PAINTSTRUCT	ps;
			HDC			hdc;
			RECT		rc;
			LOGFONT		lf;
			HFONT		hf, hfOri;

			memset(&lf, 0, sizeof(lf));
			lf.lfCharSet = GB2312_CHARSET;
			lf.lfHeight = -12;
			_tcscpy(lf.lfFaceName, TEXT("宋体"));
			lf.lfWeight = FW_MEDIUM;
			hf = CreateFontIndirect(&lf);

			hdc = BeginPaint(hwnd, &ps);
			GetClientRect(hwnd, &rc);
			GetWindowText(hwnd, szMsg, sizeof(szMsg) - 1);
			SetBkMode(hdc, TRANSPARENT);
			SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
			hfOri = (HFONT)SelectObject(hdc, hf);
			DrawText(hdc, szMsg, (int)_tcslen(szMsg), &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			SelectObject(hdc, hfOri);
			EndPaint(hwnd, &ps);

			DeleteObject(hf);
			return	0;
		}

	default:
		return	DefWindowProc(hwnd, uMsg, wParam, lParam);
	}
}

/**	显示系统信息窗口
 *	参数：
 *		message		显示的信息，当字符串为0时，为关闭窗口
 */
void ShowWaitingMessage(const TCHAR *message, HINSTANCE instance, int min_time)
{
	static TCHAR class_name[] = TEXT("WAITINGMSGWINDOW");
	static HINSTANCE instance_save;
	static HWND	hwnd = 0;
	static last_time = 0;
	WNDCLASS wc;

	if (message)
	{
		if (hwnd)
		{
			//保证显示的时间
			Sleep(min_time);
			DestroyWindow(hwnd);
			UnregisterClass(class_name, instance_save);
		}

		instance_save = instance;

		wc.style			= CS_SAVEBITS | CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc		= WaitingWindowProcedure;
		wc.cbClsExtra		= 0;
		wc.cbWndExtra		= 0;
		wc.hInstance		= instance;
		wc.hIcon			= 0;
		wc.hCursor			= LoadCursor(NULL, IDC_ARROW);
		wc.hbrBackground	= (HBRUSH) (COLOR_BTNFACE + 1);
		wc.lpszMenuName		= NULL;
		wc.lpszClassName	= class_name;

		RegisterClass(&wc);

		hwnd = CreateWindowEx(
			WS_EX_DLGMODALFRAME | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
			class_name, message, WS_POPUP | WS_DISABLED,
			0, 0, 0, 0, 0, 0, instance, 0);

		ShowWindow(hwnd, SW_SHOWNA);
		UpdateWindow(hwnd);
		last_time = GetCurrentTicks();
	}
	else
	{
		Sleep(min_time);
		DestroyWindow(hwnd);
		UnregisterClass(class_name, instance_save);
	}
}

/**	压缩文件
 *	参数：
 *		name			被压缩文件名
 *		tag_name		压缩文件名
 *	返回：
 *		1：成功，0：失败
 */
int CompressFile(const char *name, const char *tag_name)
{
	gzFile	fw;
	FILE	*fr;
	char	buffer[0x400];
	int		length;
	int		total_length = 0;

	fr = fopen(tag_name, "rb");
	if (!fr)
		return 0;

	fw = gzopen(name, "wb");
	if (!fr)
	{
		fclose(fr);
		return 0;
	}

	while((length = (int)fread(buffer, 1, sizeof(buffer), fr)) > 0)
	{
		if (gzwrite(fw, buffer, length) < 0)
		{
			length = -1;
			break;
		}
	}

	gzclose(fw);
	fclose(fr);

	if (length < 0)
	{
		_unlink(tag_name);
		return 0;
	}

	return 1;
}

/**	对可能的压缩文件进行解压缩
 *	参数：
 *		name		压缩文件名字
 *		tag_name	目标文件名字
 *		stop_length	停止长度（-1为全部解开, -2为全部解开并做CRC校验）
 *	返回：
 *		1：成功，0：失败
 */
int UncompressFile(const TCHAR *name, const TCHAR *tag_name, int stop_length)
{
	gzFile	fr;
	FILE	*fw;
	char	buffer[0x400];
	int		length;
	int		total_length = 0;
	uLong   origincrc, crc;
	BYTE    crcs[0x4];
	char	zip_file[MAX_PATH];

	Utf16ToAnsi((wchar_t*)name, zip_file, MAX_PATH);
	fr = gzopen((char *)zip_file, "rb");

	if (!fr)
		return 0;

	fw = _tfopen(tag_name, TEXT("wb"));
	if (!fw)
	{
		gzclose(fr);
		return 0;
	}

	//初始化CRC
	if (-2 == stop_length)
	  crc = crc32(0L, Z_NULL, 0);

	while((length = gzread(fr, buffer, sizeof(buffer))) > 0)
	{
		//生成数据文件的CRC
		if (-2 == stop_length)
		  crc = crc32(crc, buffer, length);

		if (length == fwrite(buffer, 1, length, fw))
		{
			total_length += length;
			if (stop_length > 0 && total_length >= stop_length)
				break;
			continue;
		}

		length = -1;
		break;
	}

	fclose(fw);
	gzclose(fr);

	//获得原始的CRC
	if (-2 == stop_length && 0 == length)
	{
    	fw = _tfopen(name, TEXT("rb"));
	    if (!fw)
		   return 0;

		//最后8个字节存放原始文件CRC32和原始文件数据长度的低32位
        fseek(fw, -8L, SEEK_END);
		fread(crcs, 1, 4, fw);

	    fclose(fw);

		origincrc = crcs[0] + crcs[1] * 0x100 + crcs[2] * 0x10000 + crcs[3] * 0x1000000;

		//如果两个crc的值不同，认为文件被损坏了
		if (crc != origincrc)
			return 0;
	}

	if (length < 0)
	{
		_tunlink(tag_name);
		return 0;
	}

	return 1;
}

/**	简单加密，加密方法: ^0xA5 >>> 3
 *	参数：
 *		x			8位数值
 *	返回：
 *		编码后的数值
 */
int SimpleEncode(char x)
{
	unsigned char v;

	v = (unsigned char)x;
	v ^= 0xA5;
	v = (v >> 3) | (v << 5);
	return (int)v;
}

/**	简单解码
 *	参数：
 *		x			8位数值
 *	返回：
 *		编码后的数值
 */
char SimpleDecode(char x)
{
	unsigned char v;

	v = (unsigned char)x;
	v = (v >> 5) | (v << 3);
	v ^= 0xA5;
	return (char)v;
}

/**	将字符串转换为16进制的可见字符串（用于URL的表达）
 *	返回：
 *		16进制字符串长度，0为有错误（长度不足）
 */
int ArrayToHexString(const char *src, int src_length, char *tag, int tag_length)
{
	static char d2h[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
	int i;

	if (tag_length < src_length * 2 + 1)
		return 0;		//长度不足

	for (i = 0; i < src_length; i++)
	{
		tag[i * 2] = d2h[SimpleEncode(src[i]) / 0x10];
		tag[i * 2 + 1] = d2h[SimpleEncode(src[i]) % 0x10];
	}

	tag[i * 2] = 0;
	return i * 2;
}

/**	将16进制字符串转换为正常的字符串（用于URL解析）
 *	返回：
 *		字符串长度，0为有错误（长度不足，或不是正确的16进制表达）
 */
int HexStringToArray(const char *src, char *tag, int tag_length)
{
	int src_length;
	int i;

	src_length = (int)strlen(src);
	if (tag_length * 2 < src_length + 1)
		return 0;

	//判断是否为正确的16进制字符串
	for (i = 0; i < src_length; i++)
	{
		if (src[i] >= '0' && src[i] <= '9')
			continue;

		if (src[i] >= 'a' && src[i] <= 'f')
			continue;

		if (src[i] >= 'A' && src[i] <= 'F')
			continue;
	}

	if (i != src_length)
		return 0;

	for (i = 0; i < src_length; i += 2)
	{
		int b0 = src[i] <= '9' ? src[i] - '0' :
			src[i] <= 'F' ? src[i] - 'A' + 10 :
			src[i] - 'a' + 10;

		int b1 = src[i + 1] <= '9' ? src[i + 1] - '0' :
			src[i + 1] <= 'F' ? src[i + 1] - 'A' + 10 :
			src[i + 1] - 'a' + 10;

		tag[i / 2] = b0 * 16 + b1;
		tag[i / 2] = SimpleDecode(tag[i / 2]);
	}

	tag[i / 2] = 0;
	return i / 2;
}

/**	获得字符串的签名。签名方法：用累加和方法进行，最后^5AA5A55A
 */
int GetSign(const char *buffer, int buffer_length)
{
	int sum = 0;
	int i;

	for (i = 0; i < buffer_length; i++)
		sum += (unsigned char)buffer[i];

	sum ^= 0x5aa5a55a;
	return sum;
}

volatile int thread_entered = 0;
volatile int lock_count = 0;

/**	加锁
 */
void Lock()
{
	//int id = GetCurrentThreadId();

	//Vista + IE7 不同的TAB之间的线程机制不清楚，如果加入锁，则会造成死锁
	//目前去掉，观察其他程序是否正常，尤其是Explorer在设置默认输入法情况下
	//的表现。 2007-7-16
	return;

	//do
	//{
	//	if (id == thread_entered)
	//		break;

	//	if (InterlockedCompareExchange(&thread_entered, id, 0) == 0)
	//		break;

	//	Sleep(10);
	//}while(1);

	//lock_count++;
}

/**	释放锁
 */
void Unlock()
{
	//int id = GetCurrentThreadId();

	return;

	//if (id != thread_entered)
	//	return;

	//lock_count--;
	//if (lock_count == 0)
	//	thread_entered = 0;
}

int is_explorer = 0;

/**	获得后端使用的程序名称
 */
const TCHAR *GetProgramName()
{
	static TCHAR name[MAX_PATH];
	TCHAR *p = name;
	TCHAR *p_name;

	if (!GetModuleFileName(0, name, _SizeOf(name)))
		return 0;

	Log(LOG_ID, L"Process Name:%s", name);

	//找到最后一个\，然后截取
	p_name = p;
	while(*p)
	{
		if (*p == '\\')
			p_name = p + 1;

		p++;
	}

	Log(LOG_ID, L"Process Name:%s", p_name);

	is_explorer = !_tcsicmp(p_name, L"explorer.exe");

	return p_name;
}

/**	获得操作系统的版本号
 */
int GetWindowVersion()
{
	OSVERSIONINFOEX osvi;
	BOOL bOsVersionInfoEx;

	ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);

	if( !(bOsVersionInfoEx = GetVersionEx ((OSVERSIONINFO *) &osvi)) )
	{
		osvi.dwOSVersionInfoSize = sizeof (OSVERSIONINFO);
		if (! GetVersionEx ( (OSVERSIONINFO *) &osvi) )
			return 0;
	}

	return osvi.dwMajorVersion << 8 | osvi.dwMinorVersion;
}

//判断是否全屏状态
int IsFullScreen()
{
	HWND hwnd;
	RECT rcWindow;
	HMONITOR hm;
	MONITORINFO mi;

	if (is_explorer)
		return 0;

	hwnd = GetForegroundWindow();

	GetWindowRect(hwnd, &rcWindow);

	hm = MonitorFromRect(&rcWindow, MONITOR_DEFAULTTONULL);

	if (!hm)
		return 0;

	mi.cbSize = sizeof(mi);

	GetMonitorInfo(hm, &mi);

	return EqualRect(&rcWindow, &mi.rcMonitor);
}


/**	将过长的字符串压缩到buffer中，压缩长度为缓冲区的大小
 *	压缩方式：在中间加入4个.两头保持。
 *	如：     北京紫光华宇软件股份有限公司
 *	压缩为： 北京紫光华....有限公司
 *	返回：
 *		压缩后的长度
 */
int PackStringToBuffer(TCHAR *str, int str_len, TCHAR *buffer, int buf_len)
{
	int index, last_index;
	int side_length;

	if (str_len <= buf_len)
	{
		_tcscpy_s(buffer, buf_len + 1, str);
		return str_len;
	}

	side_length = buf_len / 2 - 2;

	//找到边界
	last_index = index = 0;
	while(index < side_length)
	{
		last_index = index;
		index++;
	};

	if (index == side_length)
		last_index = index;

	for (index = 0; index < last_index; index++)
		*buffer++ = str[index];

	*buffer++ = '.';
	*buffer++ = '.';
	*buffer++ = '.';
	*buffer++ = '.';

	while(index < str_len - side_length)
		index++;

	for (; index < str_len; index++)
		*buffer++ = str[index];

	*buffer++ = 0;

	return (int)_tcslen(buffer);
}

int IsNumberString(TCHAR *candidate_str)
{
	static TCHAR *NumberString = TEXT("零一壹二贰三叁四肆五伍六陆七柒八捌九玖十拾百佰千仟万亿");

	int i;
	int len = (int)_tcslen(candidate_str);

	if (len < 2)
		return 0;

	for (i = 0; i < len; i++)
	{
		if (!_tcschr(NumberString, candidate_str[i]))
			return 0;
	}

	return 1;
}

int LastCharIsAtChar(TCHAR *str)
{
	int len = (int)_tcslen(str);

	if (len < 2)
		return 0;

	if (str[len - 2] != '@' && isalpha(str[len - 2]) && str[len - 1] == '@')
		return 1;
	else
		return 0;
}

char strMatch(char *src, char * pattern)
{
	int i, j, ilen1, ilen2, ipos, isfirst;

	//初始化各种变量
	ilen1 = (int)strlen(src);
	ilen2 = (int)strlen(pattern);

	i = j = isfirst = 0;
	ipos  = -1;

	//比对到字符串末尾退出
	while(i < ilen1 && j < ilen2)
	{
		//如果匹配，则i、j向后移动
		if(tolower(src[i]) == tolower(pattern[j]) || pattern[j] == '?')
        {
            ++i;
            ++j;
        }
		//如果遇到*，则j向后移动，记录*号出现位置，并设置isfirst为1（避免死循环）
		else if(pattern[j] == '*')
		{
			++j;
			ipos	= j;
			isfirst	= 1;
		}
		//在有星号的情况下，如果不匹配，则退回j，并i向后移动
		else if(ipos >= 0)
		{
			if (isfirst)
				isfirst = 0;
			else
				++i;

			j = ipos;
		}
		//不匹配、无*号，则退出
		else
			return 0;
	}

	//pattern用尽，则表示匹配
	if (j == ilen2)
		return 1;

	//pattern没用尽，src用尽了，如果pattern剩下的都是*，则匹配
	else if (i == ilen1)
	{
		//这里临时借用isfirst、ipos变量，不新定义变量了
		isfirst = 1;

		for (ipos = j; ipos < ilen2; ipos++)
		{
			if (pattern[ipos] != '*')
			{
				isfirst = 0;
				break;
			}
		}

		return isfirst;
	}
	else
		return 0;
}

int IsNumpadKey(int virtual_key)
{
	switch (virtual_key & 0xFF)
	{
		case 0x61:	case 0x62:	case 0x63:	case 0x64:	case 0x65:	
		case 0x66:	case 0x67:	case 0x68:	case 0x69:
			return 1;
			break;

		default:
			return 0;
	}
}

BOOL IsIME64()
{
	//其实下面的方法只能判断IME本身被编译为32位还是
	//64位的(对应Solution Platform中的x86或x64)，而
	//不是系统是32位还是64位。但由于我们在输入法打
	//包时实际上会编译两个版本的IME，在不同的系统上
	//安装时会自动选择不同方式编译的IME，所以实际上
	//二者保持一致
	BOOL bRes = FALSE;
	void *ptr;

	if (sizeof(ptr) == 8)
	{
		bRes = TRUE;
	}

	return bRes;
}
