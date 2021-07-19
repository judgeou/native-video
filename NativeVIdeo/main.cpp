#include <stdio.h>
#include <vector>
#include <list>
#include <string>
#include <chrono>
#include <thread>
#include <map>
#include <memory>
#include <regex>

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
#include <dxgi1_4.h>

#include <d2d1.h>
#pragma comment(lib, "d2d1.lib")
#include <dwrite.h>
#pragma comment(lib, "dwrite.lib")

#include <DirectXMath.h>
namespace dx = DirectX;

#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include "VertexShader.h"
#include "PixelShader.h"
#include "PixelShader_Subtitle.h"

#include "AudioPlayer.h"
#include "CustomTextRenderer.h"

using Microsoft::WRL::ComPtr;

using std::vector;
using std::list;
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
	AVSubtitle sub;
	double duration; // second
};

string w2s(const wstring& wstr) {
	int len = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), wstr.size(), NULL, 0, NULL, NULL);
	string str(len, '\0');
	WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), wstr.size(), &str[0], str.size(), NULL, NULL);
	return str;
}

wstring u8tow(const string& str) {
	int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.size(), NULL, 0);
	wstring wstr(len, '\0');
	MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.size(), &wstr[0], wstr.size());
	return wstr;
}

std::vector<std::wstring> split(const std::wstring& str, const std::wstring& regex_str)
{
	std::wregex regexz(regex_str);
	std::vector<std::wstring> list(std::wsregex_token_iterator(str.begin(), str.end(), regexz, -1),
		std::wsregex_token_iterator());
	return list;
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

struct Subtitle {
	std::wstring text;
	double timeleft; // 剩余时间
};

struct DecoderParam
{
	AVFormatContext* fmtCtx;
	AVCodecContext* vcodecCtx;
	AVCodecContext* acodecCtx;
	AVCodecContext* subcodecCtx;
	int width;
	int height;
	int videoStreamIndex;
	int audioStreamIndex;
	int subtitleStreamIndex;
	std::map<int, AVCodecContext*> codecMap;
	shared_ptr<nv::AudioPlayer> audioPlayer;

	double subtitleTimeBase;
	float durationSecond;
	float currentSecond;
	bool isJumpProgress;
	int playStatus;
	system_clock::time_point mouseStopTime;
	float audioVolume;
};

struct ScenceParam {
	ComPtr<ID3D11Buffer> pVertexBuffer;
	ComPtr<ID3D11Buffer> pIndexBuffer;
	ComPtr<ID3D11Buffer> pConstantBuffer;
	ComPtr<ID3D11Buffer> pConstantBufferSub;
	ComPtr<ID3D11InputLayout> pInputLayout;
	ComPtr<ID3D11VertexShader> pVertexShader;

	ComPtr<ID3D11Texture2D> texture;
	ComPtr<ID3D11Texture2D> subTexture;
	HANDLE sharedHandle;
	ComPtr<ID3D11ShaderResourceView> srvY;
	ComPtr<ID3D11ShaderResourceView> srvUV;
	ComPtr<ID3D11ShaderResourceView> subSrv;

	ComPtr<ID3D11SamplerState> pSampler;
	ComPtr<ID3D11PixelShader> pPixelShader;
	ComPtr<ID3D11PixelShader> pPixelShader_Subtitle;

	ComPtr<ID3D11BlendState> blendState;

	const UINT16 indices[6]{ 0,1,2, 0,2,3 };

	int viewWidth;
	int viewHeight;
	bool triggerFullScreen;
	DXGI_MODE_DESC1 fullScreenModeDesc;

	list<Subtitle> subtitles;

	// D2D
	ComPtr<ID2D1Factory> d2dfa;
	ComPtr<IDWriteFactory> m_pDWriteFactory;
	ComPtr<IDWriteTextFormat> textFormat;
	ComPtr<ID2D1RenderTarget> d2drt;
	ComPtr<DWriteColorTextRenderer::CustomTextRenderer> textRenderer;
};

void CreateD2DRenderTarget(ID2D1Factory* d2dfa, ID3D11Texture2D* texture, ID2D1RenderTarget** d2drt) {
	ComPtr<IDXGISurface> sur;
	texture->QueryInterface(sur.GetAddressOf());

	auto d2dPixelFormat = D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED);
	auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, d2dPixelFormat);
	d2dfa->CreateDxgiSurfaceRenderTarget(sur.Get(), &props, d2drt);
}

void CreateSubTexture(ID3D11Device* d3ddevice, int width, int height, ID3D11Texture2D** subTexture, ID3D11ShaderResourceView** srv) {
	D3D11_TEXTURE2D_DESC subDesc = {};
	subDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	subDesc.ArraySize = 1;
	subDesc.MipLevels = 1;
	subDesc.SampleDesc = { 1, 0 };
	subDesc.Width = width;
	subDesc.Height = height;
	subDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

	d3ddevice->CreateTexture2D(&subDesc, NULL, subTexture);

	// 创建着色器资源
	D3D11_SHADER_RESOURCE_VIEW_DESC const srvDesc = CD3D11_SHADER_RESOURCE_VIEW_DESC(
		*subTexture,
		D3D11_SRV_DIMENSION_TEXTURE2D,
		subDesc.Format
	);

	d3ddevice->CreateShaderResourceView(
		*subTexture,
		&srvDesc,
		srv
	);
}

void CreateTextFormat(IDWriteFactory* m_pDWriteFactory, int height, IDWriteTextFormat** textFormat) {
	FLOAT fontSize = height * 0.0567;

	m_pDWriteFactory->CreateTextFormat(
		L"Source Han Sans HW SC VF",
		NULL,
		DWRITE_FONT_WEIGHT_DEMI_BOLD,
		DWRITE_FONT_STYLE_NORMAL,
		DWRITE_FONT_STRETCH_NORMAL,
		fontSize,
		L"", //locale
		textFormat
	);
	auto p_textFormat = *textFormat;
	p_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
	p_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_FAR);
}

void SetSubtitlesNextState (list<Subtitle>& subtitles, double second) {
	for (auto i = subtitles.begin(); i != subtitles.end();) {
		auto& sub = *i;
		sub.timeleft -= second;

		if (sub.timeleft <= 0) {
			subtitles.erase(i++);
		}
		else {
			i++;
		}
	}
}

void InitDecoder(const char* filePath, DecoderParam& param) {
	AVFormatContext* fmtCtx = nullptr;
	avformat_open_input(&fmtCtx, filePath, NULL, NULL);
	avformat_find_stream_info(fmtCtx, NULL);

	AVCodecContext* vcodecCtx = nullptr;
	AVCodecContext* acodecCtx = nullptr;
	AVCodecContext* subcodecCtx = nullptr;
	for (int i = 0; i < fmtCtx->nb_streams; i++) {
		const AVCodec* codec = avcodec_find_decoder(fmtCtx->streams[i]->codecpar->codec_id);
		if (codec) {
			switch (codec->type) {
			case AVMEDIA_TYPE_VIDEO: {
				param.videoStreamIndex = i;
				param.vcodecCtx = vcodecCtx = avcodec_alloc_context3(codec);
				avcodec_parameters_to_context(vcodecCtx, fmtCtx->streams[i]->codecpar);
				avcodec_open2(vcodecCtx, codec, NULL);
				param.codecMap[i] = vcodecCtx;
				break;
			}
			case AVMEDIA_TYPE_AUDIO: {
				param.audioStreamIndex = i;
				param.acodecCtx = acodecCtx = avcodec_alloc_context3(codec);
				avcodec_parameters_to_context(acodecCtx, fmtCtx->streams[i]->codecpar);
				avcodec_open2(acodecCtx, codec, NULL);
				param.codecMap[i] = acodecCtx;

				// 初始化 AudioPlayer，无论如何固定使用双声道
				param.audioPlayer = make_shared<nv::AudioPlayer>(2, acodecCtx->sample_rate);
				param.audioPlayer->Start();
				constexpr float defaultVolume = 0.5;
				param.audioPlayer->SetVolume(defaultVolume);
				param.audioVolume = defaultVolume;
				break;
			}
			case AVMEDIA_TYPE_SUBTITLE: {
				if (subcodecCtx == nullptr) {
					param.subtitleStreamIndex = i;
					subcodecCtx = param.subcodecCtx = avcodec_alloc_context3(codec);
					avcodec_parameters_to_context(subcodecCtx, fmtCtx->streams[i]->codecpar);
					avcodec_open2(subcodecCtx, codec, NULL);
					param.codecMap[i] = subcodecCtx;

					auto timebase = fmtCtx->streams[i]->time_base;
					param.subtitleTimeBase = (double)timebase.num / timebase.den;

					if (subcodecCtx->extradata) {
						auto subinfo = u8tow((char*)subcodecCtx->extradata);
						subinfo.size();
					}
				}
				break;
			}
			}
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
		if (ret == 0 && (packet->stream_index == param.videoStreamIndex || packet->stream_index == param.audioStreamIndex || packet->stream_index == param.subtitleStreamIndex)) {
			auto codecCtx = param.codecMap[packet->stream_index];
			if (codecCtx) {
				if (codecCtx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
					AVSubtitle sub = {};
					int got_sub_ptr = 0;
					avcodec_decode_subtitle2(codecCtx, &sub, &got_sub_ptr, packet);
					auto duration = packet->duration * param.subtitleTimeBase;
					av_packet_unref(packet);

					if (got_sub_ptr) {
						return { AVMEDIA_TYPE_SUBTITLE, nullptr, sub, duration };
					}
					else {
						return { AVMEDIA_TYPE_UNKNOWN, nullptr };
					}

				}

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
	device->CreateBuffer(&cbd, &csd, &param.pConstantBufferSub);

	// 顶点着色器
	D3D11_INPUT_ELEMENT_DESC ied[] = {
		{"POSITION", 0, DXGI_FORMAT::DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT::DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
	};

	device->CreateInputLayout(ied, std::size(ied), g_main_VS, sizeof(g_main_VS), &param.pInputLayout);
	device->CreateVertexShader(g_main_VS, sizeof(g_main_VS), nullptr, &param.pVertexShader);

	AVPixelFormat pixelForamt = decoderParam.vcodecCtx->pix_fmt;

	// 纹理创建
	static const std::map<AVPixelFormat, DXGI_FORMAT> textureForamtMap = {
		{ AV_PIX_FMT_YUV420P, DXGI_FORMAT_NV12 },
		{ AV_PIX_FMT_YUV420P10, DXGI_FORMAT_P010 },
		{ AV_PIX_FMT_YUV444P10LE, DXGI_FORMAT_P010 }
	};
	D3D11_TEXTURE2D_DESC tdesc = {};
	tdesc.Format = textureForamtMap.at(pixelForamt);
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
	static const std::map<AVPixelFormat, DXGI_FORMAT> srvYForamt = {
		{ AV_PIX_FMT_YUV420P, DXGI_FORMAT_R8_UNORM },
		{ AV_PIX_FMT_YUV420P10, DXGI_FORMAT_R16_UNORM },
		{ AV_PIX_FMT_YUV444P10LE, DXGI_FORMAT_R16_UNORM }
	};
	static const std::map<AVPixelFormat, DXGI_FORMAT> srvUVFormat = {
		{ AV_PIX_FMT_YUV420P, DXGI_FORMAT_R8G8_UNORM },
		{ AV_PIX_FMT_YUV420P10, DXGI_FORMAT_R16G16_UNORM },
		{ AV_PIX_FMT_YUV444P10LE, DXGI_FORMAT_R16_UNORM }
	};

	D3D11_SHADER_RESOURCE_VIEW_DESC const YPlaneDesc = CD3D11_SHADER_RESOURCE_VIEW_DESC(
		param.texture.Get(),
		D3D11_SRV_DIMENSION_TEXTURE2D,
		srvYForamt.at(pixelForamt)
	);

	device->CreateShaderResourceView(
		param.texture.Get(),
		&YPlaneDesc,
		&param.srvY
	);

	D3D11_SHADER_RESOURCE_VIEW_DESC const UVPlaneDesc = CD3D11_SHADER_RESOURCE_VIEW_DESC(
		param.texture.Get(),
		D3D11_SRV_DIMENSION_TEXTURE2D,
		srvUVFormat.at(pixelForamt)
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
	device->CreatePixelShader(g_main_PS_Sub, sizeof(g_main_PS_Sub), nullptr, &param.pPixelShader_Subtitle);

	// 创建透明混合状态
	D3D11_BLEND_DESC omDesc = {};
	omDesc.RenderTarget[0].BlendEnable = true;
	omDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	omDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	omDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	omDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	omDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	omDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	omDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

	device->CreateBlendState(&omDesc, param.blendState.GetAddressOf());

	// imgui
	ImGui_ImplDX11_Init(device, ctx);

	// D2D
#ifdef _DEBUG
	D2D1_FACTORY_OPTIONS d2dopts[] = { D2D1_DEBUG_LEVEL::D2D1_DEBUG_LEVEL_INFORMATION };
#else
	D2D1_FACTORY_OPTIONS d2dopts[] = { D2D1_DEBUG_LEVEL::D2D1_DEBUG_LEVEL_NONE };
#endif

	D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), d2dopts, (void**)param.d2dfa.GetAddressOf());
	DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &param.m_pDWriteFactory);
	CreateTextFormat(param.m_pDWriteFactory.Get(), param.viewHeight, &param.textFormat);
	CreateSubTexture(device, param.viewWidth, param.viewHeight, &param.subTexture, &param.subSrv);
	CreateD2DRenderTarget(param.d2dfa.Get(), param.subTexture.Get(), &param.d2drt);
	param.textRenderer = new DWriteColorTextRenderer::CustomTextRenderer(param.d2dfa, param.d2drt);
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

	// 全屏窗口控制
	auto isEnterDown = io.KeysDownDuration[VK_RETURN] == 0.0f;
	if (isEnterDown) {
		param.triggerFullScreen = true;
	}

	// 滚轮可以调整音量
	auto& audioVolume = decoderParam.audioVolume;
	if (io.MouseWheel != 0) {
		audioVolume += io.MouseWheel * 0.05;
		if (audioVolume < 0) audioVolume = 0;
		if (audioVolume > 1) audioVolume = 1;
		decoderParam.audioPlayer->SetVolume(audioVolume);
	}

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
					decoderParam.audioPlayer->Stop();
					playStatus = 1;
				}
			}
			else if (playStatus == 1 || playStatus == 2) {
				if (ImGui::Button("Play")) {
					decoderParam.audioPlayer->Start();
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

		if (ImGui::Begin("Volume")) {
			ImGui::PushItemWidth(50);
			if (ImGui::VSliderFloat("", { 18, 160 }, &decoderParam.audioVolume, 0, 1, "")) {
				decoderParam.audioPlayer->SetVolume(audioVolume);
			}
			ImGui::PopItemWidth();
		}
		ImGui::End();
	}

	ImGui::Render();
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

// 切换全屏状态
void SwitchFullScreen(IDXGISwapChain3* swapchain, ScenceParam& param) {
	DXGI_SWAP_CHAIN_DESC swapDesc;
	swapchain->GetDesc(&swapDesc);
	auto& bufferDesc = swapDesc.BufferDesc;

	BOOL state;
	swapchain->GetFullscreenState(&state, NULL);
	if (state) {
		swapchain->SetFullscreenState(FALSE, NULL);
		swapchain->ResizeBuffers(
			0,
			param.viewWidth, param.viewHeight,
			DXGI_FORMAT_UNKNOWN,
			0);
	}
	else {
		swapchain->SetFullscreenState(TRUE, NULL);
		swapchain->ResizeBuffers(
			0,
			param.fullScreenModeDesc.Width, param.fullScreenModeDesc.Height,
			DXGI_FORMAT_UNKNOWN,
			0);
	}
}

void Draw(
	ID3D11Device* device, ID3D11DeviceContext* ctx, IDXGISwapChain3* swapchain,
	ScenceParam& param, DecoderParam& decoderParam
) {
	if (param.triggerFullScreen) {
		param.triggerFullScreen = false;

		SwitchFullScreen(swapchain, param);
	}
	else {
		// 必要时重新创建交换链
		DXGI_SWAP_CHAIN_DESC swapDesc;
		swapchain->GetDesc(&swapDesc);
		auto& bufferDesc = swapDesc.BufferDesc;
		if (bufferDesc.Width != param.viewWidth || bufferDesc.Height != param.viewHeight) {
			swapchain->ResizeBuffers(swapDesc.BufferCount, param.viewWidth, param.viewHeight, bufferDesc.Format, swapDesc.Flags);
		}
	}

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

	// 输出合并
	ComPtr<ID3D11Texture2D> backBuffer;
	swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);

	D3D11_TEXTURE2D_DESC backDesc;
	backBuffer->GetDesc(&backDesc);

	CD3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc(D3D11_RTV_DIMENSION_TEXTURE2D, backDesc.Format);
	ComPtr<ID3D11RenderTargetView>  rtv;
	device->CreateRenderTargetView(backBuffer.Get(), &renderTargetViewDesc, &rtv);
	ID3D11RenderTargetView* rtvs[] = { rtv.Get() };
	ctx->OMSetRenderTargets(1, rtvs, nullptr);

	const FLOAT black[] = { 0, 0, 0, 1 };
	ctx->ClearRenderTargetView(rtv.Get(), black);

	// Draw Call
	auto indicesSize = std::size(param.indices);
	ctx->DrawIndexed(indicesSize, 0, 0);

	// Set BlendState
	ctx->OMSetBlendState(param.blendState.Get(), 0, 0xFFFFFFFF);

	// Draw subTexture
	ID3D11ShaderResourceView* srvs2[] = { param.subSrv.Get() };
	ID3D11Buffer* cbs2[] = { param.pConstantBufferSub.Get() };
	ctx->VSSetConstantBuffers(0, 1, cbs2);
	ctx->PSSetShader(param.pPixelShader_Subtitle.Get(), 0, 0);
	ctx->PSSetShaderResources(0, std::size(srvs2), srvs2);
	ctx->DrawIndexed(indicesSize, 0, 0);

	DrawImgui(device, ctx, swapchain, param, decoderParam);
}

void UpdateVideoTexture(AVFrame* frame, const ScenceParam& scenceParam, const DecoderParam& decoderParam) {
	ID3D11Texture2D* t_frame = (ID3D11Texture2D*)frame->data[0];
	int t_index = (int)frame->data[1];

	D3D11_TEXTURE2D_DESC desc;
	t_frame->GetDesc(&desc);

	ComPtr<ID3D11Device> device;
	t_frame->GetDevice(device.GetAddressOf());

	ComPtr<ID3D11DeviceContext> deviceCtx;
	device->GetImmediateContext(&deviceCtx);

	ComPtr<ID3D11Texture2D> videoTexture;
	device->OpenSharedResource(scenceParam.sharedHandle, __uuidof(ID3D11Texture2D), (void**)&videoTexture);

	deviceCtx->CopySubresourceRegion(videoTexture.Get(), 0, 0, 0, 0, t_frame, t_index, 0);
	deviceCtx->Flush();
}

void UpdateSubtitlesTexture(ScenceParam& param) {
	auto& d2drt = param.d2drt;
	d2drt->BeginDraw();
	d2drt->Clear(D2D1::ColorF(0, 0, 0, 0));

	ComPtr<ID2D1SolidColorBrush> brushWhite;
	param.d2drt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brushWhite);

	auto& subtitles = param.subtitles;
	for (auto i = subtitles.begin(); i != subtitles.end();) {
		auto& sub = *i;
		if (sub.timeleft <= 0) {
			subtitles.erase(i++);
		}
		else {
			auto pos = D2D1::RectF(0, 0, param.viewWidth, param.viewHeight);
			wstring& text = sub.text;
			
			ComPtr<IDWriteTextLayout> textLayout;
			param.m_pDWriteFactory->CreateTextLayout(text.c_str(), text.size(), param.textFormat.Get(), param.viewWidth, param.viewHeight, &textLayout);

			textLayout->Draw(0, param.textRenderer.Get(), 0, 0);
			i++;
		}
	}
	d2drt->EndDraw();
}

void AddSubtitles(ScenceParam& param, AVSubtitle& sub, double duration) {
	if (sub.format == 1) {
		int num = sub.num_rects;

		for (int i = 0; i < num; i++) {
			auto rect = sub.rects[i];
			wstring ass = u8tow(rect->ass);
			auto assItems = split(ass, L",");

			if (assItems.size() >= 10) {
				wstring& text = assItems[9];
				param.subtitles.push_back({ text, duration });
			}
			
		}
	}
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

int WINAPI WinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPSTR lpCmdLine,
	_In_ int nShowCmd
) {
	CoInitializeEx(NULL, COINIT::COINIT_MULTITHREADED);
	SetProcessDPIAware();

	auto className = L"MyWindow";
	WNDCLASSW wndClass = {};
	wndClass.hInstance = hInstance;
	wndClass.lpszClassName = className;
	wndClass.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
		ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
		auto scenceParam = (ScenceParam*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

		switch (msg)
		{
		case WM_SIZE:
		{
			if (scenceParam) {
				auto width = GET_X_LPARAM(lParam);
				auto height = GET_Y_LPARAM(lParam);

				scenceParam->viewWidth = width;
				scenceParam->viewHeight = height;
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

	int windowWidth = 1280;
	int windowHeight = 720;
	auto window = CreateWindow(className, L"Hello World", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hInstance, NULL);
	ShowWindow(window, SW_SHOW);

	auto filePath = w2s(AskVideoFilePath());
	if (filePath == "") {
		return -1;
	}

	DecoderParam decoderParam = {};
	ScenceParam scenceParam = {};

	InitDecoder(filePath.c_str(), decoderParam);

	auto& width = decoderParam.width;
	auto& height = decoderParam.height;
	auto& fmtCtx = decoderParam.fmtCtx;
	auto& vcodecCtx = decoderParam.vcodecCtx;

	RECT clientRect;
	GetClientRect(window, &clientRect);
	int clientWidth = clientRect.right - clientRect.left;
	int clientHeight = clientRect.bottom - clientRect.top;

	SetWindowLongPtr(window, GWLP_USERDATA, (LONG_PTR)&scenceParam);

	// D3D11
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width = clientWidth;
	swapChainDesc.Height = clientHeight;
	swapChainDesc.Format = DXGI_FORMAT::DXGI_FORMAT_R10G10B10A2_UNORM;
	swapChainDesc.Stereo = FALSE;
	swapChainDesc.SampleDesc = { 1, 0 };
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = 2;
	swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	swapChainDesc.Flags = 0;

	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#ifdef DEBUG
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif // DEBUG

	ComPtr<ID3D11Device> d3ddeivce;
	ComPtr<ID3D11DeviceContext> d3ddeviceCtx;
	D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, NULL, NULL, D3D11_SDK_VERSION, &d3ddeivce, NULL, &d3ddeviceCtx);

	ComPtr<IDXGIDevice2> pDXGIDevice;
	d3ddeivce->QueryInterface(__uuidof(IDXGIDevice2), (void**)&pDXGIDevice);
	ComPtr<IDXGIAdapter3> pDXGIAdapter;
	pDXGIDevice->GetParent(__uuidof(IDXGIAdapter3), (void**)&pDXGIAdapter);
	ComPtr<IDXGIFactory4> pIDXGIFactory;
	pDXGIAdapter->GetParent(__uuidof(IDXGIFactory4), (void**)&pIDXGIFactory);

	ComPtr<IDXGISwapChain1> swapChain1;
	pIDXGIFactory->CreateSwapChainForHwnd(d3ddeivce.Get(), window, &swapChainDesc, NULL, NULL, &swapChain1);
	ComPtr<IDXGISwapChain3> swapChain3;
	swapChain1->QueryInterface<IDXGISwapChain3>(&swapChain3);

	// swapChain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);

	ComPtr<IDXGIOutput> pIDXGIOutput;
	swapChain3->GetContainingOutput(&pIDXGIOutput);
	ComPtr<IDXGIOutput1> pIDXGIOutput1;
	pIDXGIOutput->QueryInterface<IDXGIOutput1>(&pIDXGIOutput1);

	DXGI_OUTPUT_DESC outputDesc;
	pIDXGIOutput1->GetDesc(&outputDesc);
	DXGI_MODE_DESC1 modeDesc = {};
	modeDesc.Format = swapChainDesc.Format;
	modeDesc.Width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
	modeDesc.Height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
	modeDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;

	pIDXGIOutput1->FindClosestMatchingMode1(&modeDesc, &modeDesc, 0);
	scenceParam.fullScreenModeDesc = modeDesc;

	scenceParam.viewWidth = clientWidth;
	scenceParam.viewHeight = clientHeight;

	auto imguiCtx = ImGui::CreateContext();
	ImGui_ImplWin32_Init(window);

	InitScence(d3ddeivce.Get(), d3ddeviceCtx.Get(), scenceParam, decoderParam);

	// 屏幕刷新率
	auto displayFreq = (double)modeDesc.RefreshRate.Numerator / modeDesc.RefreshRate.Denominator;

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

			while (frameCount == 1 || (freqRatio < countRatio && decoderParam.playStatus == 0)) {
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

				if (mediaFrame.type == AVMEDIA_TYPE_UNKNOWN) {
					break;
				}

				if (mediaFrame.type == AVMEDIA_TYPE_VIDEO) {
					frameCount++;
					countRatio = (double)displayCount / frameCount;

					decoderParam.currentSecond = frameCount / frameFreq;

					if (freqRatio >= countRatio) {
						UpdateVideoTexture(frame, scenceParam, decoderParam);
						SetSubtitlesNextState(scenceParam.subtitles, 1 / frameFreq);
						UpdateSubtitlesTexture(scenceParam);
					}
				}
				else if (mediaFrame.type == AVMEDIA_TYPE_AUDIO) {
					if (frame->format == AV_SAMPLE_FMT_FLTP) {
						decoderParam.audioPlayer->WriteFLTP((float*)frame->data[0], (float*)frame->data[1], frame->nb_samples);
					}
					else if (frame->format == AV_SAMPLE_FMT_S16) {
						decoderParam.audioPlayer->WriteS16((short*)frame->data[0], frame->nb_samples);
					}
				}
				else if (mediaFrame.type == AVMEDIA_TYPE_SUBTITLE) {
					auto& sub = mediaFrame.sub;
					auto& duration = mediaFrame.duration;
					AddSubtitles(scenceParam, sub, duration);
					avsubtitle_free(&sub);
				}

				av_frame_free(&mediaFrame.frame);
			}

			if (scenceParam.viewWidth > 0 && scenceParam.viewHeight > 0) {
				Draw(d3ddeivce.Get(), d3ddeviceCtx.Get(), swapChain3.Get(), scenceParam, decoderParam);
			}
			

			pIDXGIOutput1->WaitForVBlank();
			swapChain3->Present(0, 0);

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
