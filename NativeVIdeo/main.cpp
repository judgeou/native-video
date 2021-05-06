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

#include "VertexShader.h"
#include "PixelShader.h"

using Microsoft::WRL::ComPtr;

using std::vector;
using std::string;
using std::wstring;

using namespace std::chrono;

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

void Draw(ID3D11Device* device, ID3D11DeviceContext* ctx, IDXGISwapChain* swapchain) {
	// 顶点输入
	struct Vertex {
		float x; float y; float z;
	};

	const Vertex vertices[] = {
		{-1,	1,	0},
		{1,		1,	0},
		{1,		-1,	0},
		{-1,	-1,	0},
	};

	D3D11_BUFFER_DESC bd = {};
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	bd.ByteWidth = sizeof(vertices);
	bd.StructureByteStride = sizeof(Vertex);
	D3D11_SUBRESOURCE_DATA sd = {};
	sd.pSysMem = vertices;

	ComPtr<ID3D11Buffer> pVertexBuffer;
	device->CreateBuffer(&bd, &sd, &pVertexBuffer);

	UINT stride = sizeof(Vertex);
	UINT offset = 0u;
	ID3D11Buffer* vertexBuffers[] = { pVertexBuffer.Get() };
	ctx->IASetVertexBuffers(0, 1, vertexBuffers, &stride, &offset);

	//  顶点索引
	const UINT16 indices[] = {
		0,1,2, 0,2,3
	};

	auto indicesSize = std::size(indices);
	D3D11_BUFFER_DESC ibd = {};
	ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
	ibd.ByteWidth = sizeof(indices);
	ibd.StructureByteStride = sizeof(UINT16);
	D3D11_SUBRESOURCE_DATA isd = {};
	isd.pSysMem = indices;

	ComPtr<ID3D11Buffer> pIndexBuffer;
	device->CreateBuffer(&ibd, &isd, &pIndexBuffer);
	ctx->IASetIndexBuffer(pIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);

	// 告诉系统我们画的是三角形
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// 顶点着色器
	D3D11_INPUT_ELEMENT_DESC ied[] = {
		{"POSITION", 0, DXGI_FORMAT::DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}
	};
	ComPtr<ID3D11InputLayout> pInputLayout;
	device->CreateInputLayout(ied, std::size(ied), g_main_VS, sizeof(g_main_VS), &pInputLayout);
	ctx->IASetInputLayout(pInputLayout.Get());

	ComPtr<ID3D11VertexShader> pVertexShader;
	device->CreateVertexShader(g_main_VS, sizeof(g_main_VS), nullptr, &pVertexShader);
	ctx->VSSetShader(pVertexShader.Get(), 0, 0);

	// 光栅化
	D3D11_VIEWPORT viewPort = {};
	viewPort.TopLeftX = 0;
	viewPort.TopLeftY = 0;
	viewPort.Width = 1280;
	viewPort.Height = 720;
	viewPort.MaxDepth = 1;
	viewPort.MinDepth = 0;
	ctx->RSSetViewports(1, &viewPort);

	// 像素着色器
	ComPtr<ID3D11PixelShader> pPixelShader;
	device->CreatePixelShader(g_main_PS, sizeof(g_main_PS), nullptr, &pPixelShader);
	ctx->PSSetShader(pPixelShader.Get(), 0, 0);

	// 输出合并
	ComPtr<ID3D11Texture2D> backBuffer;
	swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);

	CD3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc(D3D11_RTV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM);
	ComPtr<ID3D11RenderTargetView>  rtv;
	device->CreateRenderTargetView(backBuffer.Get(), &renderTargetViewDesc, &rtv);
	ID3D11RenderTargetView* rtvs[] = { rtv.Get() };
	ctx->OMSetRenderTargets(1, rtvs, nullptr);

	// Draw Call
	ctx->DrawIndexed(indicesSize, 0, 0);

	// 呈现
	swapchain->Present(1, 0);
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
	bufferDesc.Format = DXGI_FORMAT::DXGI_FORMAT_R8G8B8A8_UNORM;
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
			
			Draw(d3ddeivce.Get(), d3ddeviceCtx.Get(), swapChain.Get());

		}
	}

	ReleaseDecoder(decoderParam);

	CoUninitialize();
	return 0;
}
