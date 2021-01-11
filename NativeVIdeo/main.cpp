#include <stdio.h>
#include <vector>
#include <Windows.h>

using std::vector;

struct Color_RGB
{
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

void StretchBits (HWND hwnd, const vector<Color_RGB>& bits, int width, int height) {
	auto hdc = GetDC(hwnd);
	BITMAPINFO bitinfo = {};
	auto& bmiHeader = bitinfo.bmiHeader;
	bmiHeader.biSize = sizeof(bitinfo.bmiHeader);
	bmiHeader.biWidth = width;
	bmiHeader.biHeight = height;
	bmiHeader.biPlanes = 1;
	bmiHeader.biBitCount = 24;
	bmiHeader.biCompression = BI_RGB;

	StretchDIBits(hdc, 0, 0, width, height, 0, 0, width, height, &bits[0], &bitinfo, DIB_RGB_COLORS, SRCCOPY);
	ReleaseDC(hwnd, hdc);
}

int WINAPI WinMain (
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPSTR lpCmdLine,
	_In_ int nShowCmd
) {
	SetProcessDPIAware();

	auto className = L"MyWindow";
	WNDCLASSW wndClass = {};
	wndClass.hInstance = hInstance;
	wndClass.lpszClassName = className;
	wndClass.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
		return DefWindowProc(hwnd, msg, wParam, lParam);
	};

	RegisterClass(&wndClass);

	int width = 800;
	int height = 600;
	auto window = CreateWindow(className, L"Hello World БъЬт", WS_OVERLAPPEDWINDOW, 0, 0, width, height, NULL, NULL, hInstance, NULL);

	ShowWindow(window, SW_SHOW);

	vector<Color_RGB> bits(width * height, { 255, 0, 0 });
	StretchBits(window, bits, width, height);

	MSG msg;
	while (GetMessage(&msg, window, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return 0;
}
