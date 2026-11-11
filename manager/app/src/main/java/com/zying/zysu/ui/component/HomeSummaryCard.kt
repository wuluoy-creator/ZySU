package com.zying.zysu.ui.component

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.border
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.zying.zysu.ui.theme.getCardColors
import com.zying.zysu.ui.theme.AppearancePreferences
import com.zying.zysu.ui.theme.getCardShape
import com.zying.zysu.ui.theme.getCardBorder
import com.zying.zysu.ui.theme.getCardElevation

/** A compact, fully tappable summary; its details live in a native dialog. */
@Composable
internal fun HomeSummaryCard(
    title: String,
    icon: ImageVector,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    subtitle: String? = null,
    description: String? = null,
    supportingText: String? = null,
    descriptionColor: Color = Color.Unspecified,
) {
    ElevatedCard(
        onClick = onClick,
        modifier = modifier
            .heightIn(min = 136.dp)
            .border(getCardBorder(), getCardShape()),
        shape = getCardShape(),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
        elevation = getCardElevation(),
    ) {
        Column(
            modifier = Modifier.fillMaxWidth().padding(18.dp),
            horizontalAlignment = Alignment.Start,
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            FlowRow(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
                itemVerticalAlignment = Alignment.CenterVertically,
            ) {
                HomeCardIcon(icon)
                Column(
                    modifier = Modifier.weight(1f),
                    verticalArrangement = Arrangement.spacedBy(2.dp),
                ) {
                    Text(title, style = MaterialTheme.typography.titleMedium)
                    if (!subtitle.isNullOrBlank()) {
                        Text(
                            subtitle,
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
            if (!description.isNullOrBlank()) {
                Text(
                    description,
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (descriptionColor == Color.Unspecified) MaterialTheme.colorScheme.onSurfaceVariant else descriptionColor,
                )
            }
            if (!supportingText.isNullOrBlank()) {
                Text(
                    supportingText,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

/** Decorative icon treatment shared by the three home status cards. */
@Composable
internal fun HomeCardIcon(
    icon: ImageVector,
    modifier: Modifier = Modifier,
    tint: Color = MaterialTheme.colorScheme.primary,
    containerColor: Color = MaterialTheme.colorScheme.surfaceContainerHighest,
) {
    YukiIconBadge(icon = icon, modifier = modifier, tint = tint, containerColor = containerColor)
}

/** Small title treatment for the tappable half-width home cards. */
@Composable
internal fun HomeStatusCardHeader(
    title: String,
    description: String,
    icon: ImageVector,
    enabled: Boolean,
) {
    FlowRow(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
        itemVerticalAlignment = Alignment.CenterVertically,
    ) {
        Surface(
            modifier = Modifier.size(32.dp),
            shape = RoundedCornerShape(12.dp),
            color = MaterialTheme.colorScheme.surfaceContainerHighest,
            contentColor = if (enabled) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
        ) {
            Box(contentAlignment = Alignment.Center) {
                YukiIcon(icon, null, Modifier.size(20.dp))
            }
        }
        Text(
            title,
            modifier = Modifier.clearAndSetSemantics {
                heading()
                contentDescription = description
            },
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold,
        )
    }
}

/** One clear installation summary above a pair of compact detail tiles. */
@Composable
internal fun HomeStatusLayout(
    modifier: Modifier = Modifier,
    listLayout: Boolean = false,
    status: @Composable (Modifier) -> Unit,
    daemon: @Composable (Modifier) -> Unit,
    kernel: @Composable (Modifier) -> Unit,
) {
    val largeText = LocalDensity.current.fontScale * AppearancePreferences.fontScale >= 1.3f
    BoxWithConstraints(modifier.fillMaxWidth()) {
        val stack = listLayout || largeText || maxWidth < 300.dp
        Column(
            modifier = Modifier.fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            status(Modifier.fillMaxWidth())
            if (stack) {
                daemon(Modifier.fillMaxWidth())
                kernel(Modifier.fillMaxWidth())
            } else {
                Row(
                    modifier = Modifier.fillMaxWidth().height(IntrinsicSize.Min),
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    daemon(Modifier.weight(1f).fillMaxHeight())
                    kernel(Modifier.weight(1f).fillMaxHeight())
                }
            }
        }
    }
}
