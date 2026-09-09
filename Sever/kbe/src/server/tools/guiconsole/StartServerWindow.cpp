// StartServerWindow.cpp : implementation file
//

#include "stdafx.h"
#include "guiconsole.h"
#include "StartServerWindow.h"
#include "StartServerLayoutWindow.h"
#include "ClusterClient.h"

// CStartServerWindow dialog

IMPLEMENT_DYNAMIC(CStartServerWindow, CDialog)

CStartServerWindow::CStartServerWindow(CWnd* pParent /*=NULL*/)
	: CDialog(CStartServerWindow::IDD, pParent)
{

}

CStartServerWindow::~CStartServerWindow()
{
}

void CStartServerWindow::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_LIST1, m_list);
	DDX_Control(pDX, IDC_COMBO3, m_layoutlist);
	DDX_Control(pDX, IDC_LIST2, m_list1);
}


BEGIN_MESSAGE_MAP(CStartServerWindow, CDialog)
	ON_BN_CLICKED(IDC_BUTTON4, &CStartServerWindow::OnBnClickedButton4)
	ON_BN_CLICKED(IDC_BUTTON2, &CStartServerWindow::OnBnClickedButton2)
	ON_BN_CLICKED(IDC_BUTTON3, &CStartServerWindow::OnBnClickedButton3)
//	ON_CBN_SELCHANGE(IDC_COMBO3, &CStartServerWindow::OnCbnSelchangeCombo3)
ON_CBN_SELCHANGE(IDC_COMBO3, &CStartServerWindow::OnCbnSelchangeCombo3)
ON_NOTIFY(NM_THEMECHANGED, IDC_COMBO3, &CStartServerWindow::OnNMThemeChangedCombo3)
END_MESSAGE_MAP()


// CStartServerWindow message handlers
BOOL CStartServerWindow::OnInitDialog()
{
	CDialog::OnInitDialog();
	
	DWORD dwStyle = m_list.GetExtendedStyle();
	dwStyle |= LVS_EX_FULLROWSELECT;					//选中某行使整行高亮（只适用与report风格的listctrl）
	dwStyle |= LVS_EX_GRIDLINES;						//网格线（只适用与report风格的listctrl）
	//dwStyle |= LVS_EX_ONECLICKACTIVATE;
	m_list.SetExtendedStyle(dwStyle);					//设置扩展风格

	dwStyle = m_list1.GetExtendedStyle();
	dwStyle |= LVS_EX_FULLROWSELECT;					//选中某行使整行高亮（只适用与report风格的listctrl）
	dwStyle |= LVS_EX_GRIDLINES;						//网格线（只适用与report风格的listctrl）
	//dwStyle |= LVS_EX_ONECLICKACTIVATE;
	m_list1.SetExtendedStyle(dwStyle);					//设置扩展风格

	int idx = 0;
	m_list.InsertColumn(idx++, _T("componentType"),				LVCFMT_CENTER,	150);
	m_list.InsertColumn(idx++, _T("addr"),						LVCFMT_CENTER,	200);
	m_list.InsertColumn(idx++, _T("running"),					LVCFMT_CENTER,	200);

	idx = 0;
	m_list1.InsertColumn(idx++, _T("componentType"),			LVCFMT_CENTER,	200);
	m_list1.InsertColumn(idx++, _T("addr"),						LVCFMT_CENTER,	250);

	loadLayouts();
	return TRUE;  // return TRUE  unless you set the focus to a control
}

void CStartServerWindow::OnBnClickedButton4()
{
	// TODO: Add your control notification handler code here
	CStartServerLayoutWindow dlg(this);
	dlg.DoModal();
}

void CStartServerWindow::OnBnClickedButton2()
{
	CString s;

	if(m_layoutlist.GetCurSel() < 0 || m_layoutlist.GetCurSel() > m_layoutlist.GetCount())
	{
		::AfxMessageBox(L"please select a layout!");
		return;
	}

	m_layoutlist.GetLBText(m_layoutlist.GetCurSel(), s);

	char* cs = KBEngine::strutil::wchar2char(s.GetBuffer(0));
	KBEUnordered_map< std::string, std::vector<CStartServerWindow::LAYOUT_ITEM> >::iterator iter = 
		layouts_.find(cs);

	free(cs);

	if(iter == layouts_.end())
	{
		::AfxMessageBox(L"please select a layout!");
		return;
	}

	KBEngine::int32 uid = (KBEngine::int32)KBEngine::getUserUID();
	std::vector<std::string> failed;

	std::vector<CStartServerWindow::LAYOUT_ITEM>::iterator iter1 = iter->second.begin();
	for(; iter1 != iter->second.end(); iter1++)
	{
		LAYOUT_ITEM& item = (*iter1);

		KBEngine::COMPONENT_TYPE ctype = KBEngine::ComponentName2ComponentType(item.componentName.c_str());
		if(ctype == KBEngine::UNKNOWN_COMPONENT_TYPE)
			continue;

		std::vector<std::string> vec;
		KBEngine::strutil::kbe_split(item.addr, ':', vec);
		if(vec.size() != 2 || (KBEngine::uint32)inet_addr(vec[0].c_str()) == 0)
		{
			failed.push_back(item.componentName + "@" + item.addr + ": bad address");
			continue;
		}

		// 集群起停：目标为该布局主机上的 cluster 副本 servicePort。
		// 布局项里的端口原为 machine 端口，集群下以默认 servicePort 为准；
		// targetHost 让 leader 将指令路由回该主机副本的"本机代理"执行。
		KBEngine::ClusterClient::CtlResult result;
		if(!KBEngine::ClusterClient::sendServerCommand(
			KBEngine::ClusterInterface::MSG_START_SERVER, uid, (KBEngine::int32)ctype, 0, 0,
			vec[0], vec[0],
			KBEngine::ClusterInterface::DEFAULT_CLUSTER_SERVICE_PORT, result, 5000) ||
			!result.reached)
		{
			failed.push_back(item.componentName + "@" + vec[0] + ": cluster unreachable(no leader or timeout)");
			continue;
		}

		if(result.code == 0)
		{
			setRunningRow(item.componentName, item.addr, true);
		}
		else
		{
			failed.push_back(item.componentName + "@" + vec[0] + ": " + result.message);
		}
	}

	if(failed.size() > 0)
	{
		std::string all = "start server failed:\n";
		for(size_t i = 0; i < failed.size() && i < 6; i++)
			all += "  " + failed[i] + "\n";
		if(failed.size() > 6)
			all += "  ...\n";

		wchar_t* ws = KBEngine::strutil::char2wchar(all.c_str());
		::AfxMessageBox(ws);
		free(ws);
	}
}

void CStartServerWindow::OnBnClickedButton3()
{
	CString s;

	if(m_layoutlist.GetCurSel() < 0 || m_layoutlist.GetCurSel() > m_layoutlist.GetCount())
	{
		::AfxMessageBox(L"please select a layout!");
		return;
	}

	m_layoutlist.GetLBText(m_layoutlist.GetCurSel(), s);

	char* cs = KBEngine::strutil::wchar2char(s.GetBuffer(0));
	KBEUnordered_map< std::string, std::vector<CStartServerWindow::LAYOUT_ITEM> >::iterator iter = 
		layouts_.find(cs);

	free(cs);

	if(iter == layouts_.end())
	{
		::AfxMessageBox(L"please select a layout!");
		return;
	}

	KBEngine::int32 uid = (KBEngine::int32)KBEngine::getUserUID();
	std::vector<std::string> failed;

	std::vector<CStartServerWindow::LAYOUT_ITEM>::iterator iter1 = iter->second.begin();
	for(; iter1 != iter->second.end(); iter1++)
	{
		LAYOUT_ITEM& item = (*iter1);

		KBEngine::COMPONENT_TYPE ctype = KBEngine::ComponentName2ComponentType(item.componentName.c_str());
		if(ctype == KBEngine::UNKNOWN_COMPONENT_TYPE)
			continue;

		std::vector<std::string> vec;
		KBEngine::strutil::kbe_split(item.addr, ':', vec);
		if(vec.size() != 2 || (KBEngine::uint32)inet_addr(vec[0].c_str()) == 0)
		{
			failed.push_back(item.componentName + "@" + item.addr + ": bad address");
			continue;
		}

		// 集群停止：仅停止目标主机上的该类型组件(cid=0 由 leader 按注册表聚合)。
		KBEngine::ClusterClient::CtlResult result;
		if(!KBEngine::ClusterClient::sendServerCommand(
			KBEngine::ClusterInterface::MSG_STOP_SERVER, uid, (KBEngine::int32)ctype, 0, 0,
			vec[0], vec[0],
			KBEngine::ClusterInterface::DEFAULT_CLUSTER_SERVICE_PORT, result, 5000) ||
			!result.reached)
		{
			failed.push_back(item.componentName + "@" + vec[0] + ": cluster unreachable(no leader or timeout)");
			continue;
		}

		if(result.code == 0)
		{
			setRunningRow(item.componentName, item.addr, false);
		}
		else
		{
			failed.push_back(item.componentName + "@" + vec[0] + ": " + result.message);
		}
	}

	if(failed.size() > 0)
	{
		std::string all = "stop server failed:\n";
		for(size_t i = 0; i < failed.size() && i < 6; i++)
			all += "  " + failed[i] + "\n";
		if(failed.size() > 6)
			all += "  ...\n";

		wchar_t* ws = KBEngine::strutil::char2wchar(all.c_str());
		::AfxMessageBox(ws);
		free(ws);
	}
}

void CStartServerWindow::setRunningRow(const std::string& componentName, const std::string& addr, bool running)
{
	for(int row = 0; row < m_list.GetItemCount(); row++)
	{
		CString name = m_list.GetItemText(row, 0);
		CString raddr = m_list.GetItemText(row, 1);
		CString rrunning = m_list.GetItemText(row, 2);

		char* cs1 = KBEngine::strutil::wchar2char(name.GetBuffer(0));
		char* cs2 = KBEngine::strutil::wchar2char(raddr.GetBuffer(0));

		bool matched = (componentName == cs1) && (addr == cs2) &&
			((running && rrunning == L"false") || (!running && rrunning == L"true"));

		free(cs1);
		free(cs2);

		if(matched)
		{
			m_list.SetItemText(row, 2, running ? L"true" : L"false");
			break;
		}
	}
}

void CStartServerWindow::loadLayouts()
{
    CString appPath = GetAppPath();
    CString fullPath = appPath + L"\\layouts.xml";

	m_layoutlist.ResetContent();
	layouts_.clear();

	char fname[4096] = {0};

	int len = WideCharToMultiByte(CP_ACP, 0, fullPath, fullPath.GetLength(), NULL, 0, NULL, NULL);
	WideCharToMultiByte(CP_ACP,0, fullPath, fullPath.GetLength(), fname, len, NULL, NULL);
	fname[len + 1] = '\0';

	TiXmlDocument *pDocument = new TiXmlDocument(fname);
	if(pDocument == NULL || !pDocument->LoadFile(TIXML_ENCODING_UTF8))
		return;

	TiXmlElement *rootElement = pDocument->RootElement();
	TiXmlNode* node = rootElement->FirstChild();
	if(node)
	{
		do																				
		{
			std::vector<LAYOUT_ITEM>& vec = layouts_[node->Value()];
			
			wchar_t* ws = KBEngine::strutil::char2wchar(node->Value());
			m_layoutlist.AddString(ws);
			free(ws);

			TiXmlNode* childnode = node->FirstChild();
			if(childnode == NULL)
				break;
			do
			{
				LAYOUT_ITEM item;
				item.componentName = childnode->Value();
				item.addr = childnode->FirstChild()->Value();
				vec.push_back(item);
			}while((childnode = childnode->NextSibling()));		
		}while((node = node->NextSibling()));
	}

	pDocument->Clear();
	delete pDocument;
}

void CStartServerWindow::saveLayouts()
{
    //创建一个XML的文档对象。
    TiXmlDocument *pDocument = new TiXmlDocument();

	int i = 0;
	KBEUnordered_map< std::string, std::vector<LAYOUT_ITEM> >::iterator iter = layouts_.begin();
	TiXmlElement *rootElement = new TiXmlElement("root");
	pDocument->LinkEndChild(rootElement);

	for(; iter != layouts_.end(); iter++)
	{
		std::vector<LAYOUT_ITEM>::iterator iter1 = iter->second.begin();

		TiXmlElement *rootElementChild = new TiXmlElement(iter->first.c_str());
		rootElement->LinkEndChild(rootElementChild);

		for(; iter1 != iter->second.end(); iter1++)
		{
			LAYOUT_ITEM& item = (*iter1);

			TiXmlElement *rootElementChild1 = new TiXmlElement(item.componentName.c_str());
			rootElementChild->LinkEndChild(rootElementChild1);

			TiXmlText *content = new TiXmlText(item.addr.c_str());
			rootElementChild1->LinkEndChild(content);
		}
	}

    CString appPath = GetAppPath();
    CString fullPath = appPath + L"\\layouts.xml";

	char fname[4096] = {0};

	int len = WideCharToMultiByte(CP_ACP, 0, fullPath, fullPath.GetLength(), NULL, 0, NULL, NULL);
	WideCharToMultiByte(CP_ACP,0, fullPath, fullPath.GetLength(), fname, len, NULL, NULL);
	fname[len + 1] = '\0';

	pDocument->SaveFile(fname);
}
//void CStartServerWindow::OnCbnSelchangeCombo3()
//{
//	// TODO: Add your control notification handler code here
//}

void CStartServerWindow::OnCbnSelchangeCombo3()
{
	// TODO: Add your control notification handler code here
	CString s;
	if(m_layoutlist.GetEditSel() < 0 || m_layoutlist.GetEditSel() > (DWORD)m_layoutlist.GetCount())
		return;

	m_layoutlist.GetLBText(m_layoutlist.GetEditSel(), s);
	
	if(s.GetLength() <= 0 )
		return;

	m_list.DeleteAllItems();

	char* cs = KBEngine::strutil::wchar2char(s.GetBuffer(0));
	KBEUnordered_map< std::string, std::vector<LAYOUT_ITEM> >::iterator iter = layouts_.find(cs);
	free(cs);

	if(iter == layouts_.end())
	{
		return;
	}

	std::vector<LAYOUT_ITEM>::iterator iter1 = iter->second.begin();
	for(; iter1 != iter->second.end(); iter1++)
	{
		LAYOUT_ITEM& item = (*iter1);

		wchar_t* ws1 = KBEngine::strutil::char2wchar(item.componentName.c_str());
		wchar_t* ws2 = KBEngine::strutil::char2wchar(item.addr.c_str());

		m_list.InsertItem(0, ws1);
		m_list.SetItemText(0, 1, ws2);
		m_list.SetItemText(0, 2, L"false");

		free(ws1);
		free(ws2);
	}
}

void CStartServerWindow::OnNMThemeChangedCombo3(NMHDR *pNMHDR, LRESULT *pResult)
{
	// This feature requires Windows XP or greater.
	// The symbol _WIN32_WINNT must be >= 0x0501.
	// TODO: Add your control notification handler code here
	*pResult = 0;
}
