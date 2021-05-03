#include <stdio.h>
#include <vector>
#include <string>
#include <chrono>
#include <thread>

#include <Windows.h>
#include <ShlObj.h>
#include <wrl.h>

extern "C" {
#include <libavcodec/avcodec.h>
#pragma comment(lib, "avcodec.lib")

#include <libavformat/avformat.h>
#pragma comment(lib, "avformat.lib")

#include <libavutil/imgutils.h>
#pragma comment(lib, "avutil.lib")

#include <libswscale/swscale.h>
#pragma comment(lib, "swscale.lib")
}

#include <d3d9.h>
#pragma comment(lib, "d3d9.lib")

using Microsoft::WRL::ComPtr;

using std::vector;
using std::string;
using std::wstring;

using namespace std::chrono;

struct Color_RGB
{
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

string w2s(const wstring& wstr) {
	int len = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), wstr.size(), NULL, 0, NULL, NULL);
	string str(len, '\0');
	WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), wstr.size(), &str[0], str.size(), NULL, NULL);
	return str;
}

std::wstring AskVideoFilePath() {
	using Microsoft::WRL::ComPtr;

	ComPtr<IFileOpenDialog> fileDialog;

	CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
		IID_IFileOpenDialog, reinterpret_cast<void**>(fileDialog.GetAddressOf()));

	fileDialog->SetTitle(L"选择视频文件");

	COMDLG_FILTERSPEC rgSpec[] =
	{
		{ L"Video", L"*.mp4;*.mkv;*.flv;*.flac" },
	};
	fileDialog->SetFileTypes(1, rgSpec);
	fileDialog->Show(NULL);

	ComPtr<IShellItem> list;
	fileDialog->GetResult(&list);

	if (list.Get()) {
		PWSTR pszFilePath;
		list->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
		return pszFilePath;
	}
	else {
		return L"";
	}
}

void StretchBits(IDirect3DDevice9* device, const vector<uint8_t>& bits, int width, int height) {
	ComPtr<IDirect3DSurface9> surface;
	device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, surface.GetAddressOf());

	D3DLOCKED_RECT lockRect;
	surface->LockRect(&lockRect, NULL, D3DLOCK_DISCARD);

	memcpy(lockRect.pBits, &bits[0], bits.size());

	surface->UnlockRect();

	device->Present(NULL, NULL, NULL, NULL);
}

struct DecoderParam
{
	AVFormatContext* fmtCtx;
	AVCodecContext* vcodecCtx;
	int width;
	int height;
	int videoStreamIndex;
};

void InitDecoder(const char* filePath, DecoderParam& param) {
	AVFormatContext* fmtCtx = nullptr;
	avformat_open_input(&fmtCtx, filePath, NULL, NULL);
	avformat_find_stream_info(fmtCtx, NULL);

	AVCodecContext* vcodecCtx = nullptr;
	for (int i = 0; i < fmtCtx->nb_streams; i++) {
		const AVCodec* codec = avcodec_find_decoder(fmtCtx->streams[i]->codecpar->codec_id);
		if (codec->type == AVMEDIA_TYPE_VIDEO) {
			param.videoStreamIndex = i;
			vcodecCtx = avcodec_alloc_context3(codec);
			avcodec_parameters_to_context(vcodecCtx, fmtCtx->streams[i]->codecpar);
			avcodec_open2(vcodecCtx, codec, NULL);
		}
	}

	// 启用硬件解码器
	AVBufferRef* hw_device_ctx = nullptr;
	av_hwdevice_ctx_create(&hw_device_ctx, AVHWDeviceType::AV_HWDEVICE_TYPE_DXVA2, NULL, NULL, NULL);
	vcodecCtx->hw_device_ctx = hw_device_ctx;

	param.fmtCtx = fmtCtx;
	param.vcodecCtx = vcodecCtx;
	param.width = vcodecCtx->width;
	param.height = vcodecCtx->height;
}

AVFrame* RequestFrame(DecoderParam& param) {
	auto& fmtCtx = param.fmtCtx;
	auto& vcodecCtx = param.vcodecCtx;
	auto& videoStreamIndex = param.videoStreamIndex;

	while (1) {
		AVPacket* packet = av_packet_alloc();
		int ret = av_read_frame(fmtCtx, packet);
		if (ret == 0 && packet->stream_index == videoStreamIndex) {
			ret = avcodec_send_packet(vcodecCtx, packet);
			if (ret == 0) {
				AVFrame* frame = av_frame_alloc();
				ret = avcodec_receive_frame(vcodecCtx, frame);
				if (ret == 0) {
					av_packet_unref(packet);
					return frame;
				}
				else if (ret == AVERROR(EAGAIN)) {
					av_frame_unref(frame);
				}
			}
		}

		av_packet_unref(packet);
	}

	return nullptr;
}

void ReleaseDecoder(DecoderParam& param) {
	avcodec_free_context(&param.vcodecCtx);
	avformat_close_input(&param.fmtCtx);
}

void GetRGBPixels(AVFrame* frame, vector<uint8_t>& buffer, AVPixelFormat pixelFormat, int byteCount) {
	AVFrame* swFrame = av_frame_alloc();
	av_hwframe_transfer_data(swFrame, frame, 0);
	frame = swFrame;

	static SwsContext* swsctx = nullptr;
	swsctx = sws_getCachedContext(
		swsctx,
		frame->width, frame->height, (AVPixelFormat)frame->format,
		frame->width, frame->height, pixelFormat, NULL, NULL, NULL, NULL);

	uint8_t* data[] = { &buffer[0] };
	int linesize[] = { frame->width * byteCount };
	sws_scale(swsctx, frame->data, frame->linesize, 0, frame->height, data, linesize);

	av_frame_free(&swFrame);
}

int WINAPI WinMain (
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPSTR lpCmdLine,
	_In_ int nShowCmd
) {
	CoInitializeEx(NULL, COINIT::COINIT_MULTITHREADED);
	SetProcessDPIAware();

	auto filePath = w2s(AskVideoFilePath());

	auto className = L"MyWindow";
	WNDCLASSW wndClass = {};
	wndClass.hInstance = hInstance;
	wndClass.lpszClassName = className;
	wndClass.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
		switch (msg)
		{
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		default:
			return DefWindowProc(hwnd, msg, wParam, lParam);
		}
	};

	RegisterClass(&wndClass);

	DecoderParam decoderParam;
	InitDecoder(filePath.c_str(), decoderParam);
	auto& width = decoderParam.width;
	auto& height = decoderParam.height;
	auto& fmtCtx = decoderParam.fmtCtx;
	auto& vcodecCtx = decoderParam.vcodecCtx;

	vector<uint8_t> buffer(width * height * 4);

	auto window = CreateWindow(className, L"Hello World 标题", WS_OVERLAPPEDWINDOW, 0, 0, 1280, 720, NULL, NULL, hInstance, NULL);

	ShowWindow(window, SW_SHOW);

	// D3D9
	ComPtr<IDirect3D9> d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
	ComPtr<IDirect3DDevice9> d3d9Device;

	D3DPRESENT_PARAMETERS d3dParams = {};
	d3dParams.Windowed = TRUE;
	d3dParams.SwapEffect = D3DSWAPEFFECT_DISCARD;
	d3dParams.BackBufferFormat = D3DFORMAT::D3DFMT_X8R8G8B8;
	d3dParams.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
	d3dParams.BackBufferWidth = width;
	d3dParams.BackBufferHeight = height;
	d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &d3dParams, d3d9Device.GetAddressOf());

	auto currentTime = system_clock::now();

	MSG msg;
	while (1) {
		BOOL hasMsg = PeekMessage(&msg, NULL, 0, 0, PM_REMOVE);
		if (hasMsg) {
			if (msg.message == WM_QUIT) {
				break;
			}
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else {
			AVFrame* frame = RequestFrame(decoderParam);

			GetRGBPixels(frame, buffer, AVPixelFormat::AV_PIX_FMT_BGRA, 4);

			av_frame_free(&frame);

			double framerate = (double)vcodecCtx->framerate.den / vcodecCtx->framerate.num;
			std::this_thread::sleep_until(currentTime + milliseconds((int)(framerate * 1000)));
			currentTime = system_clock::now();

			StretchBits(d3d9Device.Get(), buffer, width, height);
		}
	}

	ReleaseDecoder(decoderParam);

	CoUninitialize();
	return 0;
}
