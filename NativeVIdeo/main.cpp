#include <stdio.h>
#include <Windows.h>

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

	auto hdc = GetDC(window);
	for (int x = 0; x < width; x++) {
		for (int y = 0; y < height; y++) {
			SetPixel(hdc, x, y, RGB(116, 185, 255));
		}
	}
	ReleaseDC(window, hdc);

	MSG msg;
	while (GetMessage(&msg, window, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return 0;
}
