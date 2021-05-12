#include <stdio.h>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <map>
#include <memory>

#include <Windows.h>
#include <windowsx.h>
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
#include <DirectXMath.h>
namespace dx = DirectX;

#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include "VertexShader.h"
#include "PixelShader.h"

#include "AudioPlayer.h"

using Microsoft::WRL::ComPtr;

using std::vector;
using std::string;
using std::wstring;
using std::make_shared;
using std::shared_ptr;

using namespace std::chrono;

struct Vertex {
	float x; float y; float z;
	struct
	{
		float u;
		float v;
	} tex;
};

struct MediaFrame {
	AVMediaType type;
	AVFrame* frame;
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
	AVCodecContext* acodecCtx;
	int width;
	int height;
	int videoStreamIndex;
	int audioStreamIndex;
	std::map<int, AVCodecContext*> codecMap;
	shared_ptr<nv::AudioPlayer> audioPlayer;

	float durationSecond;
	float currentSecond;
	bool isJumpProgress;
	int playStatus;
	system_clock::time_point mouseStopTime;
};

struct ScenceParam {
	ComPtr<ID3D11Buffer> pVertexBuffer;
	ComPtr<ID3D11Buffer> pIndexBuffer;
	ComPtr<ID3D11Buffer> pConstantBuffer;
	ComPtr<ID3D11InputLayout> pInputLayout;
	ComPtr<ID3D11VertexShader> pVertexShader;
	
	ComPtr<ID3D11Texture2D> texture;
	HANDLE sharedHandle;
	ComPtr<ID3D11ShaderResourceView> srvY;
	ComPtr<ID3D11ShaderResourceView> srvUV;

	ComPtr<ID3D11SamplerState> pSampler;
	ComPtr<ID3D11PixelShader> pPixelShader;

	const UINT16 indices[6]{ 0,1,2, 0,2,3 };

	int viewWidth;
	int viewHeight;
};

void InitDecoder(const char* filePath, DecoderParam& param) {
	AVFormatContext* fmtCtx = nullptr;
	avformat_open_input(&fmtCtx, filePath, NULL, NULL);
	avformat_find_stream_info(fmtCtx, NULL);

	AVCodecContext* vcodecCtx = nullptr;
	AVCodecContext* acodecCtx = nullptr;
	for (int i = 0; i < fmtCtx->nb_streams; i++) {
		const AVCodec* codec = avcodec_find_decoder(fmtCtx->streams[i]->codecpar->codec_id);
		if (codec->type == AVMEDIA_TYPE_VIDEO) {
			param.videoStreamIndex = i;
			param.vcodecCtx = vcodecCtx = avcodec_alloc_context3(codec);
			avcodec_parameters_to_context(vcodecCtx, fmtCtx->streams[i]->codecpar);
			avcodec_open2(vcodecCtx, codec, NULL);
			param.codecMap[i] = vcodecCtx;
		}
		if (codec->type == AVMEDIA_TYPE_AUDIO) {
			param.audioStreamIndex = i;
			param.acodecCtx = acodecCtx = avcodec_alloc_context3(codec);
			avcodec_parameters_to_context(acodecCtx, fmtCtx->streams[i]->codecpar);
			avcodec_open2(acodecCtx, codec, NULL);
			param.codecMap[i] = acodecCtx;

			// 初始化 AudioPlayer，无论如何固定使用双声道
			param.audioPlayer = make_shared<nv::AudioPlayer>(2, acodecCtx->sample_rate);
			param.audioPlayer->Start();
		}
	}

	// 启用硬件解码器
	AVBufferRef* hw_device_ctx = nullptr;
	av_hwdevice_ctx_create(&hw_device_ctx, AVHWDeviceType::AV_HWDEVICE_TYPE_D3D11VA, NULL, NULL, NULL);
	vcodecCtx->hw_device_ctx = hw_device_ctx;

	param.fmtCtx = fmtCtx;
	param.vcodecCtx = vcodecCtx;
	param.width = vcodecCtx->width;
	param.height = vcodecCtx->height;
}

MediaFrame RequestFrame(DecoderParam& param) {
	auto& fmtCtx = param.fmtCtx;

	while (1) {
		AVPacket* packet = av_packet_alloc();
		int ret = av_read_frame(fmtCtx, packet);
		if (ret == 0) {
			auto codecCtx = param.codecMap[packet->stream_index];
			ret = avcodec_send_packet(codecCtx, packet);
			if (ret == 0) {
				AVFrame* frame = av_frame_alloc();
				ret = avcodec_receive_frame(codecCtx, frame);
				if (ret == 0) {
					av_packet_unref(packet);
					return { codecCtx->codec_type, frame };
				}
				else if (ret == AVERROR(EAGAIN)) {
					av_frame_unref(frame);
				}
			}
		}
		else if (ret < 0) {
			return { AVMEDIA_TYPE_UNKNOWN };
		}

		av_packet_unref(packet);
	}

	return { AVMEDIA_TYPE_UNKNOWN };
}

void ReleaseDecoder(DecoderParam& param) {
	avcodec_free_context(&param.vcodecCtx);
	avformat_close_input(&param.fmtCtx);
}

void InitScence(ID3D11Device* device, ID3D11DeviceContext* ctx, ScenceParam& param, const DecoderParam& decoderParam) {
	// 顶点输入
	const Vertex vertices[] = {
		{-1,	1,	0,	0,	0},
		{1,		1,	0,	1,	0},
		{1,		-1,	0,	1,	1},
		{-1,	-1,	0,	0,	1},
	};

	D3D11_BUFFER_DESC bd = {};
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	bd.ByteWidth = sizeof(vertices);
	bd.StructureByteStride = sizeof(Vertex);
	D3D11_SUBRESOURCE_DATA sd = {};
	sd.pSysMem = vertices;

	device->CreateBuffer(&bd, &sd, &param.pVertexBuffer);

	D3D11_BUFFER_DESC ibd = {};
	ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
	ibd.ByteWidth = sizeof(param.indices);
	ibd.StructureByteStride = sizeof(UINT16);
	D3D11_SUBRESOURCE_DATA isd = {};
	isd.pSysMem = param.indices;

	device->CreateBuffer(&ibd, &isd, &param.pIndexBuffer);

	// 常量缓冲区
	auto constant = dx::XMMatrixScaling(1, 1, 1);
	constant = dx::XMMatrixTranspose(constant);
	D3D11_BUFFER_DESC cbd = {};
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	cbd.ByteWidth = sizeof(constant);
	cbd.StructureByteStride = 0;
	D3D11_SUBRESOURCE_DATA csd = {};
	csd.pSysMem = &constant;
	
	device->CreateBuffer(&cbd, &csd, &param.pConstantBuffer);

	// 顶点着色器
	D3D11_INPUT_ELEMENT_DESC ied[] = {
		{"POSITION", 0, DXGI_FORMAT::DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT::DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
	};

	device->CreateInputLayout(ied, std::size(ied), g_main_VS, sizeof(g_main_VS), &param.pInputLayout);
	device->CreateVertexShader(g_main_VS, sizeof(g_main_VS), nullptr, &param.pVertexShader);

	// 纹理创建
	D3D11_TEXTURE2D_DESC tdesc = {};
	tdesc.Format = DXGI_FORMAT_NV12;
	tdesc.Usage = D3D11_USAGE_DEFAULT;
	tdesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
	tdesc.ArraySize = 1;
	tdesc.MipLevels = 1;
	tdesc.SampleDesc = { 1, 0 };
	tdesc.Height = decoderParam.height;
	tdesc.Width = decoderParam.width;
	tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	device->CreateTexture2D(&tdesc, nullptr, &param.texture);

	// 创建纹理共享句柄
	ComPtr<IDXGIResource> dxgiShareTexture;
	param.texture->QueryInterface(__uuidof(IDXGIResource), (void**)dxgiShareTexture.GetAddressOf());
	dxgiShareTexture->GetSharedHandle(&param.sharedHandle);

	// 创建着色器资源
	D3D11_SHADER_RESOURCE_VIEW_DESC const YPlaneDesc = CD3D11_SHADER_RESOURCE_VIEW_DESC(
		param.texture.Get(),
		D3D11_SRV_DIMENSION_TEXTURE2D,
		DXGI_FORMAT_R8_UNORM
	);

	device->CreateShaderResourceView(
		param.texture.Get(),
		&YPlaneDesc,
		&param.srvY
	);

	D3D11_SHADER_RESOURCE_VIEW_DESC const UVPlaneDesc = CD3D11_SHADER_RESOURCE_VIEW_DESC(
		param.texture.Get(),
		D3D11_SRV_DIMENSION_TEXTURE2D,
		DXGI_FORMAT_R8G8_UNORM
	);

	device->CreateShaderResourceView(
		param.texture.Get(),
		&UVPlaneDesc,
		&param.srvUV
	);

	// 创建采样器
	D3D11_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = D3D11_FILTER::D3D11_FILTER_ANISOTROPIC;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.MaxAnisotropy = 16;

	device->CreateSamplerState(&samplerDesc, &param.pSampler);

	// 像素着色器
	device->CreatePixelShader(g_main_PS, sizeof(g_main_PS), nullptr, &param.pPixelShader);

	// imgui
	ImGui_ImplDX11_Init(device, ctx);
}

// 通过窗口比例与视频比例的计算，得出合适的缩放矩阵，写入常量缓冲。
void FitQuadSize(
	ID3D11DeviceContext* ctx, ID3D11Buffer* constant,
	int videoWidth, int videoHeight, int viewWidth, int viewHeight
) {
	double videoRatio = (double)videoWidth / videoHeight;
	double viewRatio = (double)viewWidth / viewHeight;
	dx::XMMATRIX matrix;

	if (videoRatio > viewRatio) {
		matrix = dx::XMMatrixScaling(1, viewRatio / videoRatio, 1);
	}
	else if (videoRatio < viewRatio) {
		matrix = dx::XMMatrixScaling(videoRatio / viewRatio, 1, 1);
	}
	else {
		matrix = dx::XMMatrixScaling(1, 1, 1);
	}
	matrix = dx::XMMatrixTranspose(matrix);

	D3D11_MAPPED_SUBRESOURCE mapped;
	ctx->Map(constant, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	memcpy(mapped.pData, &matrix, sizeof(matrix));
	ctx->Unmap(constant, 0);
}

void DrawImgui(
	ID3D11Device* device, ID3D11DeviceContext* ctx, IDXGISwapChain* swapchain,
	ScenceParam& param, DecoderParam& decoderParam
) {
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	// 这里开始写界面逻辑
	// ImGui::ShowDemoWindow();
	auto& io = ImGui::GetIO();
	auto& mouseStopTime = decoderParam.mouseStopTime;
	if (io.MouseDelta.y != 0 || io.MouseDelta.x != 0) {
		mouseStopTime = system_clock::now();
	}

	constexpr auto hideMouseDelay = 1s;
	bool isShowWidgets = ((system_clock::now() - mouseStopTime) < hideMouseDelay) || io.WantCaptureMouse;

	if (isShowWidgets) {
		if (ImGui::Begin("Play")) {
			auto& playStatus = decoderParam.playStatus;
			if (playStatus == 0) {
				if (ImGui::Button("Pause")) {
					playStatus = 1;
				}
			}
			else if (playStatus == 1 || playStatus == 2) {
				if (ImGui::Button("Play")) {
					playStatus = 0;
				}
			}
			ImGui::SameLine();

			ImGui::PushItemWidth(700);
			if (ImGui::SliderFloat("time", &decoderParam.currentSecond, 0, decoderParam.durationSecond)) {
				decoderParam.isJumpProgress = true;
			}
			ImGui::PopItemWidth();
			ImGui::SameLine();
			ImGui::Text("%.3f", decoderParam.durationSecond);
		}
		ImGui::End();
	}

	ImGui::Render();
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void Draw(
	ID3D11Device* device, ID3D11DeviceContext* ctx, IDXGISwapChain* swapchain,
	ScenceParam& param, DecoderParam& decoderParam
) {
	UINT stride = sizeof(Vertex);
	UINT offset = 0u;
	ID3D11Buffer* vertexBuffers[] = { param.pVertexBuffer.Get() };
	ctx->IASetVertexBuffers(0, 1, vertexBuffers, &stride, &offset);

	ctx->IASetIndexBuffer(param.pIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);

	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	ctx->IASetInputLayout(param.pInputLayout.Get());

	FitQuadSize(ctx, param.pConstantBuffer.Get(), decoderParam.width, decoderParam.height, param.viewWidth, param.viewHeight);
	ID3D11Buffer* cbs[] = { param.pConstantBuffer.Get() };
	ctx->VSSetConstantBuffers(0, 1, cbs);

	ctx->VSSetShader(param.pVertexShader.Get(), 0, 0);

	// 光栅化
	D3D11_VIEWPORT viewPort = {};
	viewPort.TopLeftX = 0;
	viewPort.TopLeftY = 0;
	viewPort.Width = param.viewWidth;
	viewPort.Height = param.viewHeight;
	viewPort.MaxDepth = 1;
	viewPort.MinDepth = 0;
	ctx->RSSetViewports(1, &viewPort);

	ctx->PSSetShader(param.pPixelShader.Get(), 0, 0);
	
	ID3D11ShaderResourceView* srvs[] = { param.srvY.Get(), param.srvUV.Get() };
	ctx->PSSetShaderResources(0, std::size(srvs), srvs);

	ID3D11SamplerState* samplers[] = { param.pSampler.Get() };
	ctx->PSSetSamplers(0, 1, samplers);

	// 必要时重新创建交换链
	DXGI_SWAP_CHAIN_DESC swapDesc;
	swapchain->GetDesc(&swapDesc);
	auto& bufferDesc = swapDesc.BufferDesc;
	if (bufferDesc.Width != param.viewWidth || bufferDesc.Height != param.viewHeight) {
		swapchain->ResizeBuffers(swapDesc.BufferCount, param.viewWidth, param.viewHeight, bufferDesc.Format, swapDesc.Flags);
	}

	// 输出合并
	ComPtr<ID3D11Texture2D> backBuffer;
	swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);

	CD3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc(D3D11_RTV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM);
	ComPtr<ID3D11RenderTargetView>  rtv;
	device->CreateRenderTargetView(backBuffer.Get(), &renderTargetViewDesc, &rtv);
	ID3D11RenderTargetView* rtvs[] = { rtv.Get() };
	ctx->OMSetRenderTargets(1, rtvs, nullptr);

	const FLOAT black[] = { 0, 0, 0, 1 };
	ctx->ClearRenderTargetView(rtv.Get(), black);

	// Draw Call
	auto indicesSize = std::size(param.indices);
	ctx->DrawIndexed(indicesSize, 0, 0);

	DrawImgui(device, ctx, swapchain, param, decoderParam);
}

void UpdateVideoTexture(AVFrame* frame, const ScenceParam& scenceParam, const DecoderParam& decoderParam) {
	ID3D11Texture2D* t_frame = (ID3D11Texture2D*)frame->data[0];
	int t_index = (int)frame->data[1];

	ComPtr<ID3D11Device> device;
	t_frame->GetDevice(device.GetAddressOf());

	ComPtr<ID3D11DeviceContext> deviceCtx;
	device->GetImmediateContext(&deviceCtx);

	ComPtr<ID3D11Texture2D> videoTexture;
	device->OpenSharedResource(scenceParam.sharedHandle, __uuidof(ID3D11Texture2D), (void**)&videoTexture);

	deviceCtx->CopySubresourceRegion(videoTexture.Get(), 0, 0, 0, 0, t_frame, t_index, 0);
	deviceCtx->Flush();
}

double GetFrameFreq(const DecoderParam& param) {
	auto avg_frame_rate = param.fmtCtx->streams[param.videoStreamIndex]->avg_frame_rate;
	auto framerate = param.vcodecCtx->framerate;

	if (avg_frame_rate.num > 0) {
		return (double)avg_frame_rate.num / avg_frame_rate.den;
	}
	else if (framerate.num > 0) {
		return (double)framerate.num / framerate.den;
	}
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
		ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

		switch (msg)
		{
		case WM_SIZE:
		{
			auto scenceParam = (ScenceParam*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
			if (scenceParam) {
				auto width = GET_X_LPARAM(lParam);
				auto height = GET_Y_LPARAM(lParam);

				// 专门处理从全屏恢复到窗口的特殊情况
				if ((GetWindowLongPtr(hwnd, GWL_STYLE) == (WS_VISIBLE | WS_POPUP | WS_CLIPSIBLINGS))) {
					RECT clientRect = { 0, 0, 0, 0 };
					AdjustWindowRect(&clientRect, WS_OVERLAPPEDWINDOW, FALSE);
					width = width - (clientRect.right - clientRect.left);
					height = height - (clientRect.bottom - clientRect.top);
				}

				scenceParam->viewWidth = width;
				scenceParam->viewHeight = height;
			}
			return 0;
		}
		case WM_KEYUP:
		{
			if (wParam == VK_RETURN) {
				static bool isMax = false;
				if (isMax) {
					isMax = false;
					SendMessage(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
					SetWindowLongPtr(hwnd, GWL_STYLE, WS_VISIBLE | WS_OVERLAPPEDWINDOW);
				}
				else {
					isMax = true;
					SetWindowLongPtr(hwnd, GWL_STYLE, WS_VISIBLE | WS_POPUP);
					SendMessage(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
				}
			}
			return 0;
		}
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		default:
			return DefWindowProc(hwnd, msg, wParam, lParam);
		}
	};

	RegisterClass(&wndClass);

	DecoderParam decoderParam = {};
	ScenceParam scenceParam;

	InitDecoder(filePath.c_str(), decoderParam);

	auto& width = decoderParam.width;
	auto& height = decoderParam.height;
	auto& fmtCtx = decoderParam.fmtCtx;
	auto& vcodecCtx = decoderParam.vcodecCtx;

	int windowWidth = 1280;
	int windowHeight = 720;
	auto window = CreateWindow(className, L"Hello World 标题", WS_OVERLAPPEDWINDOW, 100, 100, windowWidth, windowHeight, NULL, NULL, hInstance, NULL);
	
	RECT clientRect;
	GetClientRect(window, &clientRect);
	int clientWidth = clientRect.right - clientRect.left;
	int clientHeight = clientRect.bottom - clientRect.top;

	ShowWindow(window, SW_SHOW);
	SetWindowLongPtr(window, GWLP_USERDATA, (LONG_PTR)&scenceParam);

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
	
	scenceParam.viewWidth = clientWidth;
	scenceParam.viewHeight = clientHeight;

	auto imguiCtx = ImGui::CreateContext();
	ImGui_ImplWin32_Init(window);

	InitScence(d3ddeivce.Get(), d3ddeviceCtx.Get(), scenceParam, decoderParam);

	DEVMODE devMode = {};
	devMode.dmSize = sizeof(devMode);
	EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &devMode);
	// 屏幕刷新率
	auto displayFreq = devMode.dmDisplayFrequency;

	// 记录屏幕呈现了多少帧
	int displayCount = 1;
	// 记录视频播放了多少帧
	int frameCount = 1;

	decoderParam.durationSecond = (double)fmtCtx->duration / AV_TIME_BASE;
	auto videoTimeBase = fmtCtx->streams[decoderParam.videoStreamIndex]->time_base;
	double videoTimeBaseDouble = (double)videoTimeBase.num / videoTimeBase.den;

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
			double frameFreq = GetFrameFreq(decoderParam);
			double freqRatio = displayFreq / frameFreq;
			double countRatio = (double)displayCount / frameCount;
			
			while (freqRatio < countRatio && decoderParam.playStatus == 0) {
				if (decoderParam.isJumpProgress) {
					decoderParam.isJumpProgress = false;
					auto& current = decoderParam.currentSecond;
					int64_t jumpTimeStamp = current / videoTimeBaseDouble;
					av_seek_frame(fmtCtx, decoderParam.videoStreamIndex, jumpTimeStamp, 0);

					frameCount = current * frameFreq;
					displayCount = current * displayFreq;
				}

				auto mediaFrame = RequestFrame(decoderParam);
				auto& frame = mediaFrame.frame;

				if (frame == nullptr) {
					break;
				}

				if (mediaFrame.type == AVMEDIA_TYPE_VIDEO) {
					frameCount++;
					countRatio = (double)displayCount / frameCount;

					decoderParam.currentSecond = frameCount / frameFreq;

					if (freqRatio >= countRatio) {
						UpdateVideoTexture(frame, scenceParam, decoderParam);
					}
				}
				else if (mediaFrame.type == AVMEDIA_TYPE_AUDIO) {
					// 目前只考虑 FLTP 格式
					if (frame->format == AV_SAMPLE_FMT_FLTP) {
						decoderParam.audioPlayer->WriteFLTP((float*)frame->data[0], (float*)frame->data[1], frame->nb_samples);
					}
				}

				av_frame_free(&mediaFrame.frame);
			}

			Draw(d3ddeivce.Get(), d3ddeviceCtx.Get(), swapChain.Get(), scenceParam, decoderParam);
			
			swapChain->Present(1, 0);
			if (decoderParam.playStatus == 0) {
				displayCount++;
			}
		}
	}

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();

	ReleaseDecoder(decoderParam);

	CoUninitialize();
	return 0;
}
