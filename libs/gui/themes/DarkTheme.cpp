#include "DarkTheme.hpp"

QString DarkTheme::stylesheet() const {
    return R"(
        /* Dark Theme for Sentinel Trading Terminal */
        
        /* Tables and Data Views */
        QHeaderView::section { 
            background-color: #202225; 
            color: #E8E8E8; 
            padding: 4px;
            border: none;
            border-bottom: 1px solid #2E2E2E;
        }
        
        QTableWidget { 
            background-color: #1E1E1E; 
            color: #C0C0C0; 
            gridline-color: #2E2E2E;
            border: 1px solid #2E2E2E;
        }
        
        QTableView {
            background-color: #1E1E1E;
            color: #C0C0C0;
            gridline-color: #2E2E2E;
            border: 1px solid #2E2E2E;
            selection-background-color: #2A4A6A;
            selection-color: #FFFFFF;
        }
        
        QTableWidget::item {
            padding: 4px;
        }
        
        QTableWidget::item:selected {
            background-color: #2A4A6A;
            color: #FFFFFF;
        }
        
        /* Line Edits */
        QLineEdit {
            background-color: #2A2A2A;
            color: #E8E8E8;
            border: 1px solid #444;
            border-radius: 3px;
            padding: 4px 8px;
        }
        
        QLineEdit:focus {
            border: 1px solid #00AAFF;
            background-color: #2F2F2F;
        }
        
        /* Push Buttons */
        QPushButton {
            background-color: #404040;
            color: #E8E8E8;
            border: 1px solid #555;
            border-radius: 3px;
            padding: 6px 12px;
        }
        
        QPushButton:hover {
            background-color: #505050;
            border-color: #666;
        }
        
        QPushButton:pressed {
            background-color: #353535;
        }
        
        QPushButton:disabled {
            background-color: #2A2A2A;
            color: #666;
            border-color: #333;
        }
        
        /* Combo Boxes */
        QComboBox {
            background-color: #2A2A2A;
            color: #E8E8E8;
            border: 1px solid #444;
            border-radius: 3px;
            padding: 4px 8px;
        }
        
        QComboBox:hover {
            border-color: #555;
        }
        
        QComboBox:focus {
            border-color: #00AAFF;
        }
        
        QComboBox::drop-down {
            border: none;
            background-color: #404040;
        }
        
        QComboBox QAbstractItemView {
            background-color: #2A2A2A;
            color: #E8E8E8;
            selection-background-color: #2A4A6A;
            selection-color: #FFFFFF;
            border: 1px solid #444;
        }
        
        /* Text Edits */
        QTextEdit {
            background-color: #1A1A1A;
            color: #E0E0E0;
            border: 1px solid #2E2E2E;
        }
        
        QTextEdit:focus {
            border-color: #00AAFF;
        }
        
        /* Labels */
        QLabel {
            color: #E8E8E8;
        }
        
        /* Menu Bar */
        QMenuBar {
            background-color: #252525;
            color: #E8E8E8;
            border-bottom: 1px solid #333;
        }
        
        QMenuBar::item {
            background-color: transparent;
            padding: 4px 8px;
        }
        
        QMenuBar::item:selected {
            background-color: #353535;
        }
        
        QMenuBar::item:pressed {
            background-color: #404040;
        }
        
        /* Menus */
        QMenu {
            background-color: #2A2A2A;
            color: #E8E8E8;
            border: 1px solid #444;
        }
        
        QMenu::item {
            padding: 6px 24px 6px 24px;
        }
        
        QMenu::item:selected {
            background-color: #2A4A6A;
            color: #FFFFFF;
        }
        
        QMenu::separator {
            height: 1px;
            background-color: #444;
            margin: 4px 8px;
        }
        
        /* Scroll Bars */
        QScrollBar:vertical {
            background-color: #1E1E1E;
            width: 12px;
            border: none;
        }
        
        QScrollBar::handle:vertical {
            background-color: #404040;
            min-height: 20px;
            border-radius: 6px;
        }
        
        QScrollBar::handle:vertical:hover {
            background-color: #505050;
        }
        
        QScrollBar:horizontal {
            background-color: #1E1E1E;
            height: 12px;
            border: none;
        }
        
        QScrollBar::handle:horizontal {
            background-color: #404040;
            min-width: 20px;
            border-radius: 6px;
        }
        
        QScrollBar::handle:horizontal:hover {
            background-color: #505050;
        }
        
        QScrollBar::add-line, QScrollBar::sub-line {
            border: none;
            background: none;
        }
        
        /* Dock Widgets */
        QDockWidget {
            color: #E8E8E8;
            titlebar-close-icon: none;
            titlebar-normal-icon: none;
        }
        
        QDockWidget::title {
            background-color: #252525;
            padding: 4px;
            border-bottom: 1px solid #333;
        }
        
        /* Tabs */
        QTabWidget::pane {
            background-color: #1E1E1E;
            border: 1px solid #2E2E2E;
        }
        
        QTabBar::tab {
            background-color: #252525;
            color: #C0C0C0;
            padding: 6px 12px;
            border: 1px solid #333;
            border-bottom: none;
        }
        
        QTabBar::tab:selected {
            background-color: #1E1E1E;
            color: #E8E8E8;
            border-bottom: 1px solid #1E1E1E;
        }
        
        QTabBar::tab:hover {
            background-color: #2A2A2A;
        }
        
        /* Group Boxes */
        QGroupBox {
            color: #E8E8E8;
            border: 1px solid #444;
            border-radius: 4px;
            margin-top: 8px;
            padding-top: 8px;
        }
        
        QGroupBox::title {
            subcontrol-origin: margin;
            subcontrol-position: top left;
            padding: 0 4px;
            background-color: #1E1E1E;
        }
        
        /* Sliders */
        QSlider::groove:horizontal {
            background-color: #2A2A2A;
            height: 4px;
            border-radius: 2px;
        }
        
        QSlider::handle:horizontal {
            background-color: #00AAFF;
            width: 12px;
            height: 12px;
            margin: -4px 0;
            border-radius: 6px;
        }
        
        QSlider::handle:horizontal:hover {
            background-color: #0088CC;
        }
        
        /* Checkboxes and Radio Buttons */
        QCheckBox {
            color: #E8E8E8;
        }
        
        QCheckBox::indicator {
            width: 16px;
            height: 16px;
            border: 1px solid #555;
            background-color: #2A2A2A;
            border-radius: 3px;
        }
        
        QCheckBox::indicator:checked {
            background-color: #00AAFF;
            border-color: #00AAFF;
        }
        
        QRadioButton {
            color: #E8E8E8;
        }
        
        QRadioButton::indicator {
            width: 16px;
            height: 16px;
            border: 1px solid #555;
            border-radius: 8px;
            background-color: #2A2A2A;
        }
        
        QRadioButton::indicator:checked {
            background-color: #00AAFF;
            border-color: #00AAFF;
        }
        
        /* Tooltips */
        QToolTip {
            background-color: #2A2A2A;
            color: #E8E8E8;
            border: 1px solid #555;
            padding: 4px;
        }
    )";
}

