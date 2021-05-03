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

#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")

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

void RenderHWFrame(HWND hwnd, AVFrame* frame) {
	IDirect3DSurface9* surface = (IDirect3DSurface9*)frame->data[3];
	IDirect3DDevice9* device;
	surface->GetDevice(&device);

	static ComPtr<IDirect3DSwapChain9> mySwap;
	if (mySwap == nullptr) {
		D3DPRESENT_PARAMETERS params = {};
		params.Windowed = TRUE;
		params.hDeviceWindow = hwnd;
		params.BackBufferFormat = D3DFORMAT::D3DFMT_X8R8G8B8;
		params.BackBufferWidth = frame->width;
		params.BackBufferHeight = frame->height;
		params.SwapEffect = D3DSWAPEFFECT_DISCARD;
		params.BackBufferCount = 1;
		params.Flags = 0;
		device->CreateAdditionalSwapChain(&params, mySwap.GetAddressOf());
	}

	ComPtr<IDirect3DSurface9> backSurface;
	mySwap->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, backSurface.GetAddressOf());

	device->StretchRect(surface, NULL, backSurface.Get(), NULL, D3DTEXF_LINEAR);

	mySwap->Present(NULL, NULL, NULL, NULL, NULL);
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

	int clientWidth = 1280;
	int clientHeight = 720;
	auto window = CreateWindow(className, L"Hello World 标题", WS_POPUP, 100, 100, clientWidth, clientHeight, NULL, NULL, hInstance, NULL);

	ShowWindow(window, SW_SHOW);

	// D3D11
	DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
	auto& bufferDesc = swapChainDesc.BufferDesc;
	bufferDesc.Width = clientWidth;
	bufferDesc.Height = clientHeight;
	bufferDesc.Format = DXGI_FORMAT::DXGI_FORMAT_B8G8R8A8_UNORM;
	bufferDesc.RefreshRate.Numerator = 0;
	bufferDesc.RefreshRate.Denominator = 0;
	bufferDesc.Scaling = DXGI_MODE_SCALING_STRETCHED;
	bufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.SampleDesc.Quality = 0;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = 2;
	swapChainDesc.OutputWindow = window;
	swapChainDesc.Windowed = TRUE;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	swapChainDesc.Flags = 0;

	UINT flags = 0;

#ifdef DEBUG
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif // DEBUG

	ComPtr<IDXGISwapChain> swapChain;
	ComPtr<ID3D11Device> d3ddeivce;
	ComPtr<ID3D11DeviceContext> d3ddeviceCtx;
	D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, NULL, NULL, D3D11_SDK_VERSION, &swapChainDesc, &swapChain, &d3ddeivce, NULL, &d3ddeviceCtx);
	
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

			double framerate = (double)vcodecCtx->framerate.den / vcodecCtx->framerate.num;
			// std::this_thread::sleep_until(currentTime + milliseconds((int)(framerate * 1000)));
			currentTime = system_clock::now();

			RenderHWFrame(window, frame);

			av_frame_free(&frame);
		}
	}

	ReleaseDecoder(decoderParam);

	CoUninitialize();
	return 0;
}
