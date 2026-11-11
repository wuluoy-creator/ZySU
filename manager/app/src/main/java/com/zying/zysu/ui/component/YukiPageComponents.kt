package com.zying.zysu.ui.component

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.zying.zysu.ui.theme.CardConfig
import com.zying.zysu.ui.theme.getCardBorder
import com.zying.zysu.ui.theme.getCardShape

/** Quiet reading surfaces echo the dock's rounded silhouette without blurring scrolling lists. */
@Composable
fun YukiPanel(
    modifier: Modifier = Modifier,
    emphasized: Boolean = false,
    content: @Composable ColumnScope.() -> Unit,
) {
    val colors = MaterialTheme.colorScheme
    Surface(
        modifier = modifier.fillMaxWidth(),
        shape = getCardShape(),
        color = (if (emphasized) colors.primaryContainer else colors.surfaceContainerLow)
            .copy(alpha = CardConfig.cardAlpha),
        contentColor = if (emphasized) colors.onPrimaryContainer else colors.onSurface,
        border = getCardBorder(),
    ) { Column(content = content) }
}

/** A consistent, decorative icon holder for rows and section summaries. */
@Composable
fun YukiIconBadge(
    icon: ImageVector,
    modifier: Modifier = Modifier,
    tint: Color = MaterialTheme.colorScheme.primary,
    containerColor: Color = MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.55f),
) {
    Surface(
        modifier = modifier.size(44.dp),
        shape = RoundedCornerShape(16.dp),
        color = containerColor,
        contentColor = tint,
    ) {
        Box(contentAlignment = Alignment.Center) {
            YukiIcon(icon, contentDescription = null, modifier = Modifier.size(24.dp))
        }
    }
}

@Composable
fun YukiSectionHeading(text: String, modifier: Modifier = Modifier) {
    Text(
        text = text,
        modifier = modifier.padding(horizontal = 8.dp, vertical = 12.dp).semantics { heading() },
        style = MaterialTheme.typography.titleSmall,
        fontWeight = FontWeight.SemiBold,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}
